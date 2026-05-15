/**
 * @file progress.c
 * @brief 进度文件（pbin/spbin/fpbin）的写入、归档、恢复与生命周期管理
 *
 * 核心设计哲学：
 * - 同构分片：pbin 与 fpbin 采用完全相同的物理格式
 * - 页脚自描述：已封口分片末尾自带 Footer（magic + row_count + crc），无需外部 idx 陪伴
 * - 两阶段提交：活跃分片使用轻量 .idx 作为临时草稿，封口时"先盖钢印、再烧草稿"
 * - 崩溃恢复：Footer 优先，idx 兜底
 *
 * 进度文件格式（以 --progress-file=task1 为例）：
 * - task1.idx          原子更新的统一游标索引
 * - task1_000000.pbin  已封口的已完成记录分片
 * - task1_00000N.idx   活跃分片的临时草稿索引
 * - task1.spbin        跳过记录（熔断设备上的目录）
 * - task1.fpbin_000XXX 恢复期间隔离新发现子目录的临时分片
 * - task1.fpbin.idx    fpbin 分片的游标索引
 * - task1.archive      zlib 压缩的历史分片归档
 * - task1.config       会话配置快照
 */
#include "progress.h"
#include "utils.h"
#include "archive_format.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <zlib.h>
#include <stdint.h>
#include <dirent.h>
#include <stdatomic.h>
#include "log.h"

/* ================================================================
 * Filename helpers
 * ================================================================ */

/**
 * @brief  生成统一索引文件名（{base}.idx）
 * @param  base  const char*  进度文件前缀，不能为空
 * @return char*  动态分配的字符串，调用方负责 free
 */
char *get_index_filename(const char *base) {
    char *name = safe_malloc(strlen(base) + 32);
    sprintf(name, "%s.idx", base);
    return name;
}

/**
 * @brief  生成 pbin 分片文件名（{base}_000000.pbin）
 * @param  base   const char*   进度文件前缀，不能为空
 * @param  index  unsigned long 分片编号，取值范围: >= 0
 * @return char*  动态分配的字符串，调用方负责 free
 */
char *get_slice_filename(const char *base, unsigned long index) {
    char *name = safe_malloc(strlen(base) + 32);
    sprintf(name, "%s_%06lu.pbin", base, index);
    return name;
}

/**
 * @brief  生成归档文件名（{base}.archive）
 * @param  base  const char*  进度文件前缀，不能为空
 * @return char*  动态分配的字符串，调用方负责 free
 */
char *get_archive_filename(const char *base) {
    char *name = safe_malloc(strlen(base) + 32);
    sprintf(name, "%s.archive", base);
    return name;
}

/**
 * @brief  生成 spbin 文件名（{base}.spbin）
 * @param  base  const char*  进度文件前缀，不能为空
 * @return char*  动态分配的字符串，调用方负责 free
 */
char *get_spbin_filename(const char *base) {
    char *name = safe_malloc(strlen(base) + 32);
    sprintf(name, "%s.spbin", base);
    return name;
}

/**
 * @brief  生成按分片草稿索引文件名（{base}_000000.idx）
 * @param  base   const char*   进度文件前缀，不能为空
 * @param  index  unsigned long 分片编号，取值范围: >= 0
 * @return char*  动态分配的字符串，调用方负责 free
 */
char *get_per_slice_index_filename(const char *base, unsigned long index) {
    char *name = safe_malloc(strlen(base) + 32);
    sprintf(name, "%s_%06lu.idx", base, index);
    return name;
}

/**
 * @brief  生成 fpbin 分片文件名（{base}.fpbin_000000）
 * @param  base   const char*   进度文件前缀，不能为空
 * @param  index  unsigned long 分片编号，取值范围: >= 0
 * @return char*  动态分配的字符串，调用方负责 free
 */
char *get_fpbin_slice_filename(const char *base, unsigned long index) {
    char *name = safe_malloc(strlen(base) + 48);
    sprintf(name, "%s.fpbin_%06lu", base, index);
    return name;
}

/**
 * @brief  生成 fpbin 索引文件名（{base}.fpbin.idx）
 * @param  base  const char*  进度文件前缀，不能为空
 * @return char*  动态分配的字符串，调用方负责 free
 */
char *get_fpbin_index_filename(const char *base) {
    char *name = safe_malloc(strlen(base) + 32);
    sprintf(name, "%s.fpbin.idx", base);
    return name;
}

/**
 * @brief  将 stat::st_mode 转换为 dirent::d_type 等价值
 * @param  mode  mode_t  文件模式位
 * @return unsigned char  对应的 d_type 值（DT_REG/DT_DIR/DT_LNK/...），未知时返回 DT_UNKNOWN
 */
static unsigned char mode_to_dtype(mode_t mode) {
    if (S_ISREG(mode)) return DT_REG;
    if (S_ISDIR(mode)) return DT_DIR;
    if (S_ISLNK(mode)) return DT_LNK;
    if (S_ISCHR(mode)) return DT_CHR;
    if (S_ISBLK(mode)) return DT_BLK;
    if (S_ISFIFO(mode)) return DT_FIFO;
    if (S_ISSOCK(mode)) return DT_SOCK;
    return DT_UNKNOWN;
}

/* ================================================================
 * Footer 读写与校验
 * ================================================================ */

/**
 * @brief  向 pbin/fpbin 文件末尾写入 Footer（封口操作）
 * @param  fp        FILE*     已打开的可写文件指针，不能为空
 * @param  row_count uint64_t  该分片的实际数据行数，取值范围: >= 0
 * @return bool  返回 true 表示写入并 fsync 成功；false 表示失败
 *
 * @note   Footer 为固定 24 字节，通过 O_APPEND 原子追加到文件末尾。
 *         footer_crc32 覆盖 magic + row_count（前 16 字节）。
 *         写入后执行 fsync 确保数据落盘。
 */
bool write_pbin_footer(FILE *fp, uint64_t row_count) {
    if (!fp) return false;
    fflush(fp);
    int fd = fileno(fp);
    if (fd < 0) return false;

    PbinFooter f = {
        .magic = PBIN_FOOTER_MAGIC,
        .row_count = row_count,
        .data_crc32 = 0
    };
    /* footer_crc32 覆盖 magic + row_count（前 16 字节） */
    f.footer_crc32 = (uint32_t)crc32(0, (const Bytef *)&f.magic, (uInt)(sizeof(f.magic) + sizeof(f.row_count)));

    ssize_t w = write(fd, &f, sizeof(f));
    if (w != (ssize_t)sizeof(f)) return false;
    if (fsync(fd) != 0) return false;
    return true;
}

/**
 * @brief  从 pbin/fpbin 文件末尾读取 Footer
 * @param  path  const char*   文件路径，不能为空
 * @param  out   PbinFooter*   输出缓冲区，用于存放读取到的 Footer，不能为空
 * @return bool  返回 true 表示读取并校验成功；false 表示文件不存在、大小不足或校验失败
 */
bool read_pbin_footer(const char *path, PbinFooter *out) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;

    off_t end = lseek(fd, 0, SEEK_END);
    if (end < (off_t)sizeof(PbinFooter)) {
        close(fd);
        return false;
    }

    if (lseek(fd, end - sizeof(PbinFooter), SEEK_SET) < 0) {
        close(fd);
        return false;
    }

    ssize_t n = read(fd, out, sizeof(PbinFooter));
    close(fd);
    if (n != (ssize_t)sizeof(PbinFooter)) return false;

    return verify_pbin_footer(out);
}

/**
 * @brief  校验 Footer 的 magic 和 crc
 * @param  f  const PbinFooter*  要校验的 Footer 指针，不能为空
 * @return bool  返回 true 表示校验通过；false 表示 magic 错误或 crc 不匹配
 */
bool verify_pbin_footer(const PbinFooter *f) {
    if (f->magic != PBIN_FOOTER_MAGIC) return false;
    uint32_t expected = (uint32_t)crc32(0, (const Bytef *)&f->magic, sizeof(f->magic) + sizeof(f->row_count));
    return f->footer_crc32 == expected;
}

/**
 * @brief  获取指定 pbin 分片的行数（Footer 优先，idx 兜底）
 * @param  cfg    const Config*   全局配置指针，不能为空
 * @param  index  unsigned long   分片编号，取值范围: >= 0
 * @return unsigned long  分片行数；无法确定时返回 0
 *
 * @note   先尝试读取分片末尾 Footer，校验通过则返回 footer.row_count。
 *         若 Footer 校验失败，则尝试读取按分片草稿 idx 文件中的行数。
 */
unsigned long get_slice_row_count(const Config *cfg, unsigned long index) {
    char *slice_path = get_slice_filename(cfg->progress_base, index);
    PbinFooter f;
    if (read_pbin_footer(slice_path, &f)) {
        free(slice_path);
        return (unsigned long)f.row_count;
    }
    free(slice_path);

    /* Fallback: 读取按分片 idx */
    char *idx_path = get_per_slice_index_filename(cfg->progress_base, index);
    FILE *fp = fopen(idx_path, "r");
    unsigned long row_count = 0;
    if (fp) {
        if (fscanf(fp, "%lu", &row_count) != 1) row_count = 0;
        fclose(fp);
    }
    free(idx_path);
    return row_count;
}

/* ================================================================
 * pbin / spbin 写入
 * ================================================================ */

/**
 * @brief  向 pbin/fpbin 文件写入单条记录
 * @param  fp    FILE*             已打开的可写文件指针，不能为空
 * @param  path  const char*       文件路径，不能为空
 * @param  info  const struct stat* 文件 stat 信息指针，允许为 NULL（此时写入全 0）
 * @return void
 *
 * @note   单条记录格式：[path_len][path][dev][ino][mtime][d_type]
 *         各字段大小与平台相关（size_t、dev_t、ino_t、time_t、unsigned char）。
 */
static void write_pbin_record(FILE *fp, const char *path, const struct stat *info) {
    size_t path_len = strlen(path);
    dev_t dev = info ? info->st_dev : 0;
    ino_t ino = info ? info->st_ino : 0;
    time_t mtime = info ? info->st_mtime : 0;
    unsigned char d_type = info ? mode_to_dtype(info->st_mode) : DT_UNKNOWN;

    fwrite(&path_len, sizeof(size_t), 1, fp);
    fwrite(path, 1, path_len, fp);
    fwrite(&dev, sizeof(dev_t), 1, fp);
    fwrite(&ino, sizeof(ino_t), 1, fp);
    fwrite(&mtime, sizeof(time_t), 1, fp);
    fwrite(&d_type, sizeof(unsigned char), 1, fp);
}

/**
 * @brief  记录单条已处理路径到当前活跃 pbin 分片
 * @param  cfg   const Config*       全局配置指针，不能为空
 * @param  state RuntimeState*       运行时状态指针，不能为空
 * @param  path  const char*         文件路径，不能为空
 * @param  info  const struct stat*  文件 stat 信息指针，允许为 NULL
 * @return void
 *
 * @note   若当前无活跃分片，自动创建新的 pbin 文件和对应的 .idx 草稿。
 *         当 line_count 达到 progress_slice_lines（默认 100000）时执行分片轮转：
 *         1. 写入 Footer 封口当前分片
 *         2. 删除草稿 idx（"烧草稿"）
 *         3. 调用 process_old_slice 处理旧分片（归档或删除）
 *         4. 创建新分片并更新统一索引
 */
void record_path(const Config *cfg, RuntimeState *state, const char *path, const struct stat *info) {
    if (cfg->clean) return;  /* --clean 模式不保留任何进度文件 */
    if (!state->write_slice_file) {
        char *p = get_slice_filename(cfg->progress_base, state->write_slice_index);
        state->write_slice_file = fopen(p, "wb");
        free(p);
        if (state->write_slice_file) {
            /* 创建活跃分片的草稿 idx */
            char *idx = get_per_slice_index_filename(cfg->progress_base, state->write_slice_index);
            FILE *ifp = fopen(idx, "w");
            if (ifp) { fprintf(ifp, "0\n"); fclose(ifp); }
            free(idx);
        }
    }
    if (!state->write_slice_file) return;
    write_pbin_record(state->write_slice_file, path, info);
    state->line_count++;
    state->processed_count++;
    if (state->line_count >= cfg->progress_slice_lines) {
        /* rotate slice: 先盖钢印(Footer)，再烧草稿(idx) */
        write_pbin_footer(state->write_slice_file, state->line_count);
        fclose(state->write_slice_file);
        state->write_slice_file = NULL;

        /* 删除按分片草稿 idx */
        char *old_idx = get_per_slice_index_filename(cfg->progress_base, state->write_slice_index);
        unlink(old_idx);
        free(old_idx);

        process_old_slice(cfg, state->write_slice_index);
        state->write_slice_index++;
        state->line_count = 0;
        char *p = get_slice_filename(cfg->progress_base, state->write_slice_index);
        state->write_slice_file = fopen(p, "wb");
        free(p);
        if (state->write_slice_file) {
            char *idx = get_per_slice_index_filename(cfg->progress_base, state->write_slice_index);
            FILE *ifp = fopen(idx, "w");
            if (ifp) { fprintf(ifp, "0\n"); fclose(ifp); }
            free(idx);
        }
        atomic_update_index(cfg, state);
    }
}

/* ================================================================
 * record_path 批量缓冲
 * ================================================================ */

/**
 * @brief  初始化 RecordBatch 批量缓冲结构
 * @param  batch  RecordBatch*  指向要初始化的缓冲结构，不能为空
 * @return void
 */
void record_path_batch_init(RecordBatch *batch) {
    if (!batch) return;
    memset(batch, 0, sizeof(*batch));
}

/**
 * @brief  将批量缓冲中的所有记录刷出到 pbin 文件（内部实现）
 * @param  cfg    const Config*   全局配置指针，不能为空
 * @param  state  RuntimeState*   运行时状态指针，不能为空
 * @param  batch  RecordBatch*    批量缓冲指针，不能为空
 * @return void
 */
static void record_path_batch_flush_internal(const Config *cfg, RuntimeState *state, RecordBatch *batch) {
    if (!batch || batch->count == 0) return;
    for (int i = 0; i < batch->count; i++) {
        record_path(cfg, state, batch->paths[i], &batch->stats[i]);
        free(batch->paths[i]);
        batch->paths[i] = NULL;
    }
    batch->count = 0;
    batch->total_bytes = 0;
}

/**
 * @brief  将批量缓冲中的所有记录刷出到 pbin 文件（外部接口）
 * @param  cfg    const Config*   全局配置指针，不能为空
 * @param  state  RuntimeState*   运行时状态指针，不能为空
 * @param  batch  RecordBatch*    批量缓冲指针，不能为空
 * @return void
 */
void record_path_batch_flush(const Config *cfg, RuntimeState *state, RecordBatch *batch) {
    record_path_batch_flush_internal(cfg, state, batch);
}

/**
 * @brief  向批量缓冲追加一条记录（满时自动刷出）
 * @param  cfg    const Config*       全局配置指针，不能为空
 * @param  state  RuntimeState*       运行时状态指针，不能为空
 * @param  batch  RecordBatch*        批量缓冲指针，不能为空
 * @param  path   const char*         文件路径，不能为空
 * @param  info   const struct stat*  文件 stat 信息指针，允许为 NULL
 * @return bool  返回 true 表示追加成功；false 表示内存分配失败
 *
 * @note   当 batch->count >= RECORD_BATCH_COUNT（4096）或
 *         total_bytes + entry_size >= RECORD_BATCH_BYTES（1MB）时自动刷出。
 */
bool record_path_batch_append(const Config *cfg, RuntimeState *state, RecordBatch *batch, const char *path, const struct stat *info) {
    if (!batch || !path) return false;
    
    size_t path_len = strlen(path);
    size_t entry_size = path_len + sizeof(dev_t) + sizeof(ino_t) + sizeof(time_t) + sizeof(unsigned char);
    
    /* 检查是否需要先 flush */
    if (batch->count >= RECORD_BATCH_COUNT ||
        (batch->count > 0 && batch->total_bytes + entry_size >= RECORD_BATCH_BYTES)) {
        record_path_batch_flush_internal(cfg, state, batch);
    }
    
    if (batch->count >= RECORD_BATCH_COUNT) {
        /* flush 后仍然满（理论上不应发生，因为 RECORD_BATCH_COUNT 是硬上限） */
        record_path_batch_flush_internal(cfg, state, batch);
    }
    
    batch->paths[batch->count] = strdup(path);
    if (info) {
        batch->stats[batch->count] = *info;
    } else {
        memset(&batch->stats[batch->count], 0, sizeof(struct stat));
    }
    batch->total_bytes += entry_size;
    batch->count++;
    return true;
}

/**
 * @brief  将跳过记录（spbin）追加到磁盘文件
 * @param  cfg    const Config*   全局配置指针，不能为空
 * @param  state  RuntimeState*   运行时状态指针（当前未使用，保留接口一致性）
 * @param  entry  const SpbinEntry* 跳过记录条目指针，不能为空
 * @return void
 *
 * @note   以追加模式（"ab"）打开 {base}.spbin，写入 SpbinRecordHeader + path 字节。
 *         不执行 fsync，依赖操作系统的缓冲策略。
 */
void record_skip(const Config *cfg, RuntimeState *state, const SpbinEntry *entry) {
    (void)state;
    FILE *fp = fopen(get_spbin_filename(cfg->progress_base), "ab");
    if (!fp) return;

    SpbinRecordHeader hdr = {
        .path_len = (uint32_t)strlen(entry->path),
        .dev = entry->dev,
        .blacklist_time = entry->blacklist_time,
        .retry_count = entry->retry_count,
        .probe_interval = entry->probe_interval,
        .d_type = entry->d_type,
        .s_status = entry->s_status
    };
    fwrite(&hdr, sizeof(hdr), 1, fp);
    fwrite(entry->path, 1, hdr.path_len, fp);
    fclose(fp);
}

/* ================================================================
 * 索引与游标
 * ================================================================ */

/**
 * @brief  原子更新统一索引文件和按分片草稿索引
 * @param  cfg    const Config*   全局配置指针，不能为空
 * @param  state  RuntimeState*   运行时状态指针，不能为空
 * @return void
 *
 * @note   采用"写临时文件 + rename"的两阶段提交策略保证原子性：
 *         1. 统一索引（{base}.idx）：记录 write_slice_index、line_count、processed_count、output_slice_num、output_line_count
 *         2. 按分片草稿索引（{base}_00000N.idx）：记录当前活跃分片的 line_count
 *         临时文件命名包含线程 ID 以避免多线程冲突。
 */
void atomic_update_index(const Config *cfg, RuntimeState *state) {
    char *idx_file = get_index_filename(cfg->progress_base);
    char *tmp_file = safe_malloc(strlen(idx_file) + 64);
    snprintf(tmp_file, strlen(idx_file) + 64, "%s.tmp.%lu", idx_file, (unsigned long)pthread_self());

    FILE *tmp_fp = fopen(tmp_file, "w");
    if (tmp_fp) {
        fprintf(tmp_fp, "%lu %lu %lu %lu %lu\n",
                state->write_slice_index,
                state->line_count,
                state->processed_count,
                state->output_slice_num,
                state->output_line_count);
        fclose(tmp_fp);
        if (rename(tmp_file, idx_file) != 0) unlink(tmp_file);
    }
    free(idx_file);
    free(tmp_file);

    /* 同步更新当前活跃分片的草稿 idx */
    char *per_idx = get_per_slice_index_filename(cfg->progress_base, state->write_slice_index);
    char *per_tmp = safe_malloc(strlen(per_idx) + 64);
    snprintf(per_tmp, strlen(per_idx) + 64, "%s.tmp.%lu", per_idx, (unsigned long)pthread_self());
    FILE *ptf = fopen(per_tmp, "w");
    if (ptf) {
        fprintf(ptf, "%lu\n", state->line_count);
        fclose(ptf);
        if (rename(per_tmp, per_idx) != 0) unlink(per_tmp);
    }
    free(per_idx);
    free(per_tmp);
}

/**
 * @brief  从磁盘加载统一索引文件到运行时状态
 * @param  cfg    const Config*   全局配置指针，不能为空
 * @param  state  RuntimeState*   运行时状态指针，不能为空
 * @return bool  返回 true 表示成功加载 5 个字段；false 表示文件不存在或格式错误
 */
bool load_progress_index(const Config *cfg, RuntimeState *state) {
    char *idx_file = get_index_filename(cfg->progress_base);
    FILE *fp = fopen(idx_file, "r");
    if (!fp) { free(idx_file); return false; }
    int matches = fscanf(fp, "%lu %lu %lu %lu %lu",
            &state->write_slice_index, &state->line_count,
            &state->processed_count, &state->output_slice_num, &state->output_line_count);
    fclose(fp); free(idx_file);
    state->process_slice_index = state->write_slice_index;
    return matches == 5;
}

/* ================================================================
 * 归档 (archive with block_type)
 * ================================================================ */

/**
 * @brief  将单个分片文件压缩并追加到归档文件
 * @param  cfg         const Config*  全局配置指针，不能为空
 * @param  slice_path  const char*    要归档的分片文件路径，不能为空
 * @param  block_type  uint8_t        块类型，取值范围: ARCHIVE_BLOCK_NORMAL(0) 或 ARCHIVE_BLOCK_SPBIN(1)
 * @return void
 *
 * @note   流程：
 *         1. 读取整个分片到内存
 *         2. 若分片末尾有有效 Footer，则提取 row_count 并从数据区剔除 Footer
 *         3. 使用 zlib compress 压缩数据区
 *         4. 追加 ArchiveBlockHeader + 压缩数据到 {base}.archive
 *         5. 删除原始分片文件（unlink）
 *         若压缩失败或文件打开失败，则保留原始分片不删除。
 */
static void archive_slice_to_file(const Config *cfg, const char *slice_path, uint8_t block_type) {
    FILE *in = fopen(slice_path, "rb");
    if (!in) return;

    fseek(in, 0, SEEK_END);
    long src_size = ftell(in);
    fseek(in, 0, SEEK_SET);
    if (src_size <= 0) { fclose(in); unlink(slice_path); return; }

    unsigned char *src_buf = safe_malloc(src_size);
    if (fread(src_buf, 1, src_size, in) != (size_t)src_size) {
        free(src_buf); fclose(in); return;
    }
    fclose(in);

    long data_size = src_size;
    uint64_t row_count = 0;
    if (block_type != ARCHIVE_BLOCK_SPBIN && src_size >= (long)sizeof(PbinFooter)) {
        PbinFooter *f = (PbinFooter *)(src_buf + src_size - sizeof(PbinFooter));
        if (verify_pbin_footer(f)) {
            data_size = src_size - (long)sizeof(PbinFooter);
            row_count = f->row_count;
        }
    }

    unsigned long dest_len = compressBound((uLong)data_size);
    unsigned char *dest_buf = safe_malloc(dest_len);
    if (compress(dest_buf, &dest_len, src_buf, (uLong)data_size) != Z_OK) {
        log_error("压缩分片失败");
        free(src_buf); free(dest_buf); return;
    }
    free(src_buf);

    char *archive_path = get_archive_filename(cfg->progress_base);
    FILE *out = fopen(archive_path, "ab");
    if (out) {
        ArchiveBlockHeader bh = {
            .uncompressed_size = (uint32_t)data_size,
            .compressed_size = (uint32_t)dest_len,
            .block_type = block_type,
            .row_count = row_count
        };
        fwrite(&bh, sizeof(bh), 1, out);
        fwrite(dest_buf, 1, dest_len, out);
        fclose(out);
        unlink(slice_path);
    } else {
        perror("无法打开归档文件");
    }
    free(dest_buf);
    free(archive_path);
}

/**
 * @brief  处理已完成的旧分片（归档或删除）
 * @param  cfg    const Config*   全局配置指针，不能为空
 * @param  index  unsigned long   已完成的分片编号，取值范围: >= 0
 * @return void
 *
 * @note   若 cfg->archive 为 true，则调用 archive_slice_to_file 压缩归档；
 *         否则直接 unlink 删除分片文件。
 */
void process_old_slice(const Config *cfg, unsigned long index) {
    char *src_path = get_slice_filename(cfg->progress_base, index);
    if (cfg->archive) {
        archive_slice_to_file(cfg, src_path, ARCHIVE_BLOCK_NORMAL);
    } else {
        unlink(src_path);
    }
    free(src_path);
}

/**
 * @brief  任务结束时归档所有残留分片
 * @param  cfg    const Config*   全局配置指针，不能为空
 * @param  state  RuntimeState*   运行时状态指针，不能为空
 * @return void
 *
 * @note   流程：
 *         1. 若存在活跃分片（write_slice_file），先封口写 Footer，再归档或保留
 *         2. 归档 spbin 文件（作为 archive 的最后一个块）
 *         若未开启归档模式，保留当前活跃分片和 spbin 以供后续恢复。
 */
void finalize_archive(const Config *cfg, RuntimeState *state) {
    if (state->write_slice_file) {
        /* 正常结束时给活跃分片盖钢印 */
        write_pbin_footer(state->write_slice_file, state->line_count);
        fclose(state->write_slice_file);
        state->write_slice_file = NULL;
        /* 删除按分片草稿 idx */
        char *per_idx = get_per_slice_index_filename(cfg->progress_base, state->write_slice_index);
        unlink(per_idx);
        free(per_idx);

        char *src_path = get_slice_filename(cfg->progress_base, state->write_slice_index);
        if (cfg->archive) {
            archive_slice_to_file(cfg, src_path, ARCHIVE_BLOCK_NORMAL);
        }
        /* If not archiving, keep the current slice file for resume */
        free(src_path);
    }
    /* Archive spbin as the last block */
    char *spbin_path = get_spbin_filename(cfg->progress_base);
    if (access(spbin_path, F_OK) == 0) {
        if (cfg->archive) {
            archive_slice_to_file(cfg, spbin_path, ARCHIVE_BLOCK_SPBIN);
        }
        /* If not archiving, keep spbin for resume */
    }
    free(spbin_path);
}

/* ================================================================
 * 进度恢复 (从 archive 和散落 pbin)
 * ================================================================ */

/**
 * @brief  解析 pbin/fpbin 数据缓冲区，提取指纹并插入集合
 * @param  buf          const uint8_t*    数据缓冲区指针，不能为空
 * @param  size         size_t            缓冲区大小（字节）
 * @param  max_rows     uint64_t          最大解析行数，0 表示无限制
 * @param  visited_set  FingerprintSet*   本次任务的 visited_set（去重），允许为 NULL
 * @param  ref_set      FingerprintSet*   半增量的 reference_set，允许为 NULL
 * @param  ref_map      ReferenceMap*     半增量的 reference_map，允许为 NULL
 * @return void
 *
 * @note   按 pbin 记录格式顺序解析：path_len → path → dev → ino → mtime → d_type。
 *         对每条记录计算指纹并插入 visited_set（若提供）和 ref_set/ref_map（若提供）。
 *         当 max_rows > 0 且已解析行数达到 max_rows 时提前停止。
 */
static void parse_pbin_buffer(const uint8_t *buf, size_t size, uint64_t max_rows,
                              FingerprintSet *visited_set,
                              FingerprintSet *ref_set,
                              ReferenceMap *ref_map) {
    size_t pos = 0;
    uint64_t rows = 0;
    while (pos < size) {
        if (max_rows > 0 && rows >= max_rows) break;
        if (pos + sizeof(size_t) > size) break;
        size_t path_len;
        memcpy(&path_len, buf + pos, sizeof(size_t));
        pos += sizeof(size_t);

        size_t entry_size = path_len + sizeof(dev_t) + sizeof(ino_t) + sizeof(time_t) + sizeof(unsigned char);
        if (pos + entry_size > size) break;

        char *path_str = safe_malloc(path_len + 1);
        memcpy(path_str, buf + pos, path_len);
        path_str[path_len] = '\0';
        pos += path_len;

        dev_t dev; ino_t ino; time_t mtime; unsigned char d_type;
        memcpy(&dev, buf + pos, sizeof(dev_t)); pos += sizeof(dev_t);
        memcpy(&ino, buf + pos, sizeof(ino_t)); pos += sizeof(ino_t);
        memcpy(&mtime, buf + pos, sizeof(time_t)); pos += sizeof(time_t);
        memcpy(&d_type, buf + pos, sizeof(unsigned char)); pos += sizeof(unsigned char);

        uint8_t fp[FP_SIZE];
        fp_compute(path_str, dev, ino, fp);
        free(path_str);

        if (visited_set) fp_set_insert(visited_set, fp);
        if (ref_set) fp_set_insert(ref_set, fp);
        if (ref_map) ref_map_insert(ref_map, fp, mtime, d_type);
        rows++;
    }
}

/**
 * @brief  解析 spbin 数据缓冲区，加载跳过记录到内存缓存
 * @param  buf  const uint8_t*  数据缓冲区指针，不能为空
 * @param  size size_t          缓冲区大小（字节）
 * @param  ctx  AppContext*     应用上下文指针，不能为空
 * @return void
 *
 * @note   按 SpbinRecordHeader + path 的格式顺序解析。
 *         对每个条目：创建 SpbinEntry 追加到 ctx->spbin_entries 数组，
 *         并根据状态更新 DeviceManager（CONDEMNED 或 DEAD）和 ProbeScheduler。
 */
static void parse_spbin_buffer(const uint8_t *buf, size_t size,
                               AppContext *ctx) {
    size_t pos = 0;
    while (pos < size) {
        if (pos + sizeof(SpbinRecordHeader) > size) break;
        SpbinRecordHeader hdr;
        memcpy(&hdr, buf + pos, sizeof(hdr));
        pos += sizeof(hdr);

        if (pos + hdr.path_len > size) break;
        char *path = safe_malloc(hdr.path_len + 1);
        memcpy(path, buf + pos, hdr.path_len);
        path[hdr.path_len] = '\0';
        pos += hdr.path_len;

        SpbinEntry entry = {
            .path = path,
            .dev = hdr.dev,
            .blacklist_time = hdr.blacklist_time,
            .retry_count = hdr.retry_count,
            .probe_interval = hdr.probe_interval,
            .d_type = hdr.d_type,
            .s_status = hdr.s_status
        };
        spbin_append(ctx, &entry);

        if (ctx->dev_mgr) {
            if (hdr.s_status == SP_STATUS_CONDEMNED) {
                dev_mgr_mark_condemned(ctx->dev_mgr, hdr.dev);
            } else {
                dev_mgr_mark_dead(ctx->dev_mgr, hdr.dev);
                ProbeTask task = {
                    .dev = hdr.dev,
                    .next_probe_time = time(NULL) + hdr.probe_interval,
                    .probe_interval = hdr.probe_interval,
                    .retry_count = hdr.retry_count,
                    .s_status = hdr.s_status
                };
                safe_strcpy(task.probe_path, path, sizeof(task.probe_path));
                probe_scheduler_push(ctx->probe_scheduler, &task);
            }
        }
    }
}

/**
 * @brief  遍历归档文件，解压并解析所有块
 * @param  cfg         const Config*   全局配置指针，不能为空
 * @param  ctx         AppContext*     应用上下文指针，不能为空
 * @param  visited_set FingerprintSet* 本次任务的 visited_set，允许为 NULL
 * @param  ref_set     FingerprintSet* 半增量的 reference_set，允许为 NULL
 * @param  ref_map     ReferenceMap*   半增量的 reference_map，允许为 NULL
 * @return void
 *
 * @note   顺序读取 ArchiveBlockHeader，做 sanity check（block_type、大小上限 512MB）后，
 *         分配缓冲区、读取压缩数据、调用 uncompress 解压，再根据 block_type 分发到
 *         parse_pbin_buffer 或 parse_spbin_buffer。
 *         若某块校验失败或读取不完整，则停止继续解析。
 */
static void iterate_archive(const Config *cfg, AppContext *ctx,
                            FingerprintSet *visited_set,
                            FingerprintSet *ref_set,
                            ReferenceMap *ref_map) {
    char *archive_path = get_archive_filename(cfg->progress_base);
    FILE *fp = fopen(archive_path, "rb");
    if (!fp) { free(archive_path); return; }

    ArchiveBlockHeader bh;
    while (fread(&bh, sizeof(bh), 1, fp) == 1) {
        /* Sanity check for corrupted or old-format archive */
        if (bh.block_type != ARCHIVE_BLOCK_NORMAL && bh.block_type != ARCHIVE_BLOCK_SPBIN) break;
        if (bh.compressed_size > 512 * 1024 * 1024 || bh.uncompressed_size > 512 * 1024 * 1024) break;

        unsigned char *cmp_buf = safe_malloc(bh.compressed_size);
        if (fread(cmp_buf, 1, bh.compressed_size, fp) != bh.compressed_size) {
            free(cmp_buf); break;
        }

        unsigned char *raw_buf = safe_malloc(bh.uncompressed_size);
        unsigned long dest_len = bh.uncompressed_size;
        if (uncompress(raw_buf, &dest_len, cmp_buf, bh.compressed_size) == Z_OK) {
            if (bh.block_type == ARCHIVE_BLOCK_SPBIN) {
                parse_spbin_buffer(raw_buf, dest_len, ctx);
            } else {
                /* 归档块内是纯数据区，无 Footer */
                parse_pbin_buffer(raw_buf, dest_len, 0, visited_set, ref_set, ref_map);
            }
        }
        free(raw_buf);
        free(cmp_buf);
    }
    fclose(fp);
    free(archive_path);
}

/**
 * @brief  遍历磁盘上散落的 pbin 分片并解析
 * @param  cfg         const Config*   全局配置指针，不能为空
 * @param  state       RuntimeState*   运行时状态指针（当前未使用，保留接口一致性）
 * @param  visited_set FingerprintSet* 本次任务的 visited_set，允许为 NULL
 * @param  ref_set     FingerprintSet* 半增量的 reference_set，允许为 NULL
 * @param  ref_map     ReferenceMap*   半增量的 reference_map，允许为 NULL
 * @return void
 *
 * @note   从分片 0 开始顺序尝试打开，连续缺失超过 50 个且已超过 write_slice_index 时停止。
 *         对每个存在的分片：读取全部内容，若末尾有有效 Footer 则剔除 Footer 后解析数据区，
 *         否则解析整个文件（可能包含无效数据，但 parse_pbin_buffer 会自动防御）。
 */
static void iterate_pbin_slices(const Config *cfg, RuntimeState *state,
                                FingerprintSet *visited_set,
                                FingerprintSet *ref_set,
                                ReferenceMap *ref_map) {
    int consecutive_missing = 0;
    for (unsigned long s_idx = 0; ; ++s_idx) {
        char *slice_path = get_slice_filename(cfg->progress_base, s_idx);
        FILE *slice_fp = fopen(slice_path, "rb");
        if (!slice_fp) {
            free(slice_path);
            consecutive_missing++;
            if (consecutive_missing > 50 && s_idx > state->write_slice_index) break;
            continue;
        }
        consecutive_missing = 0;

        fseek(slice_fp, 0, SEEK_END);
        long fsize = ftell(slice_fp);
        fseek(slice_fp, 0, SEEK_SET);
        if (fsize > 0) {
            unsigned char *buf = safe_malloc(fsize);
            fread(buf, 1, fsize, slice_fp);
            long data_size = fsize;
            if (fsize >= (long)sizeof(PbinFooter)) {
                PbinFooter *f = (PbinFooter *)(buf + fsize - sizeof(PbinFooter));
                if (verify_pbin_footer(f)) {
                    data_size = fsize - (long)sizeof(PbinFooter);
                }
            }
            parse_pbin_buffer(buf, data_size, 0, visited_set, ref_set, ref_map);
            free(buf);
        }
        fclose(slice_fp);
        free(slice_path);
    }
}

/**
 * @brief  统计归档文件中 Normal 块的数量（SPBIN 块不计入）
 * @param  cfg  const Config*  全局配置指针，不能为空
 * @return unsigned long  Normal 块的数量
 */
static unsigned long count_archive_blocks(const Config *cfg) {
    char *archive_path = get_archive_filename(cfg->progress_base);
    FILE *fp = fopen(archive_path, "rb");
    if (!fp) { free(archive_path); return 0; }

    unsigned long count = 0;
    ArchiveBlockHeader bh;
    while (fread(&bh, sizeof(bh), 1, fp) == 1) {
        if (bh.block_type != ARCHIVE_BLOCK_NORMAL && bh.block_type != ARCHIVE_BLOCK_SPBIN) break;
        if (bh.compressed_size > 512 * 1024 * 1024) break;
        if (bh.block_type == ARCHIVE_BLOCK_NORMAL)
            count++;
        if (fseek(fp, bh.compressed_size, SEEK_CUR) != 0) break;
    }
    fclose(fp);
    free(archive_path);
    return count;
}

/**
 * @brief  统计磁盘上散落 pbin 分片的数量
 * @param  cfg  const Config*  全局配置指针，不能为空
 * @return unsigned long  存在的分片数量
 *
 * @note   从分片 0 开始顺序检查，连续缺失超过 50 个时停止。
 */
static unsigned long count_pbin_slices(const Config *cfg) {
    unsigned long count = 0;
    int consecutive_missing = 0;
    for (unsigned long s_idx = 0; consecutive_missing <= 50; ++s_idx) {
        char *slice_path = get_slice_filename(cfg->progress_base, s_idx);
        if (access(slice_path, F_OK) == 0) {
            count++;
            consecutive_missing = 0;
        } else {
            consecutive_missing++;
        }
        free(slice_path);
    }
    return count;
}

/* ================================================================
 * fpbin Cache (temporary buffer for new sub-dirs during pbin replay)
 * Uses flat array in memory + optional disk overflow file.
 * ================================================================ */

#define FPBIN_MEM_WATERMARK 10000

/**
 * @brief  原子更新 fpbin 索引文件（fpbin.idx）
 * @param  ctx  AppContext*  应用上下文指针，不能为空
 * @return void
 *
 * @note   记录当前 fpbin 分片号与行数，采用写临时文件 + rename 的原子更新策略。
 */
static void atomic_update_fpbin_index(AppContext *ctx) {
    char *idx_file = get_fpbin_index_filename(ctx->cfg.progress_base);
    char *tmp_file = safe_malloc(strlen(idx_file) + 64);
    snprintf(tmp_file, strlen(idx_file) + 64, "%s.tmp.%lu", idx_file, (unsigned long)pthread_self());

    FILE *tmp_fp = fopen(tmp_file, "w");
    if (tmp_fp) {
        fprintf(tmp_fp, "%lu %lu\n", ctx->fpbin_write_slice_index, ctx->fpbin_line_count);
        fclose(tmp_fp);
        if (rename(tmp_file, idx_file) != 0) unlink(tmp_file);
    }
    free(idx_file);
    free(tmp_file);
}

/**
 * @brief  打开或创建新的 fpbin 活跃分片
 * @param  ctx  AppContext*  应用上下文指针，不能为空
 * @return void
 *
 * @note   关闭当前活跃分片（如有），以 "wb" 模式打开新分片文件，重置 fpbin_line_count 为 0。
 */
static void fpbin_open_slice(AppContext *ctx) {
    if (ctx->fpbin_slice_file) {
        fclose(ctx->fpbin_slice_file);
        ctx->fpbin_slice_file = NULL;
    }
    char *p = get_fpbin_slice_filename(ctx->cfg.progress_base, ctx->fpbin_write_slice_index);
    ctx->fpbin_slice_file = fopen(p, "wb");
    free(p);
    ctx->fpbin_line_count = 0;
}

/**
 * @brief  轮转 fpbin 分片（封口当前分片并创建新分片）
 * @param  ctx  AppContext*  应用上下文指针，不能为空
 * @return void
 *
 * @note   写入 Footer 封口当前分片，关闭文件，递增分片编号，打开新分片，更新 fpbin.idx。
 */
static void fpbin_rotate_slice(AppContext *ctx) {
    if (ctx->fpbin_slice_file) {
        write_pbin_footer(ctx->fpbin_slice_file, ctx->fpbin_line_count);
        fclose(ctx->fpbin_slice_file);
        ctx->fpbin_slice_file = NULL;
    }
    ctx->fpbin_write_slice_index++;
    fpbin_open_slice(ctx);
    atomic_update_fpbin_index(ctx);
}

/**
 * @brief  向 fpbin 追加一条记录（新发现的子目录，恢复模式专用）
 * @param  ctx   AppContext*         应用上下文指针，不能为空
 * @param  path  const char*         目录路径，不能为空
 * @param  st    const struct stat*  目录 stat 信息指针，不能为空
 * @return void
 *
 * @note   当内存中条目数小于 FPBIN_MEM_WATERMARK（10000）时，缓存到内存数组；
 *         达到水位后，将内存数组批量刷出到当前 fpbin 分片文件，清空内存后继续追加。
 *         当分片行数达到 progress_slice_lines 时自动轮转。
 */
void fpbin_append(AppContext *ctx, const char *path, const struct stat *st) {
    if (!ctx->fpbin_slice_file) {
        fpbin_open_slice(ctx);
    }

    if (ctx->fpbin_count < FPBIN_MEM_WATERMARK) {
        /* Grow array if needed */
        if (ctx->fpbin_count >= ctx->fpbin_capacity) {
            size_t new_cap = ctx->fpbin_capacity ? ctx->fpbin_capacity * 2 : 1024;
            ctx->fpbin_entries = realloc(ctx->fpbin_entries, new_cap * sizeof(char *));
            ctx->fpbin_stats   = realloc(ctx->fpbin_stats,   new_cap * sizeof(struct stat));
            ctx->fpbin_capacity = new_cap;
        }
        ctx->fpbin_entries[ctx->fpbin_count] = strdup(path);
        ctx->fpbin_stats[ctx->fpbin_count]   = *st;
        ctx->fpbin_count++;
    } else {
        /* Flush memory array to current fpbin slice */
        if (ctx->fpbin_slice_file) {
            setvbuf(ctx->fpbin_slice_file, NULL, _IOFBF, 8 * 1024 * 1024);
            for (size_t i = 0; i < ctx->fpbin_count; i++) {
                write_pbin_record(ctx->fpbin_slice_file, ctx->fpbin_entries[i], &ctx->fpbin_stats[i]);
            }
            ctx->fpbin_line_count += ctx->fpbin_count;
        }
        /* Clear memory array */
        for (size_t i = 0; i < ctx->fpbin_count; i++) {
            free(ctx->fpbin_entries[i]);
        }
        ctx->fpbin_count = 0;
        /* Append current record */
        if (ctx->fpbin_slice_file) {
            write_pbin_record(ctx->fpbin_slice_file, path, st);
            ctx->fpbin_line_count++;
        }
        /* Rotate if needed */
        if (ctx->fpbin_line_count >= ctx->cfg.progress_slice_lines) {
            fpbin_rotate_slice(ctx);
        }
    }
}

/**
 * @brief  清空 fpbin 内存缓存数组
 * @param  ctx  AppContext*  应用上下文指针，不能为空
 * @return void
 */
static void fpbin_clear_mem(AppContext *ctx) {
    for (size_t i = 0; i < ctx->fpbin_count; i++) {
        free(ctx->fpbin_entries[i]);
    }
    ctx->fpbin_count = 0;
}

/**
 * @brief  查找当前最大的 pbin 分片索引
 * @param  cfg  const Config*  全局配置指针，不能为空
 * @return unsigned long  最大存在的分片编号；无分片时返回 0
 *
 * @note   从 0 开始顺序检查，连续缺失超过 50 个时停止。
 */
static unsigned long find_max_pbin_index(const Config *cfg) {
    unsigned long max_idx = 0;
    int consecutive_missing = 0;
    for (unsigned long i = 0; consecutive_missing <= 50; ++i) {
        char *slice_path = get_slice_filename(cfg->progress_base, i);
        if (access(slice_path, F_OK) == 0) {
            max_idx = i;
            consecutive_missing = 0;
        } else {
            consecutive_missing++;
        }
        free(slice_path);
    }
    return max_idx;
}

/**
 * @brief  将 fpbin 临时分片转正为正式 pbin 分片
 * @param  ctx  AppContext*  应用上下文指针，不能为空
 * @return void
 *
 * @note   转正流程（fpbin → pbin）：
 *         1. 将内存中残留条目刷出到当前 fpbin 分片
 *         2. 封口最后一个 fpbin 分片（写 Footer）
 *         3. 若从未创建过 fpbin 文件但有内存数据，创建 slice 0 并写入
 *         4. 计算 pbin 起始编号（max_pbin_index + 1），rename 所有 fpbin 分片
 *         5. 逐个读取转正后 pbin 的 Footer 进行校验
 *         6. 校验全部通过后删除 fpbin.idx 和残留 fpbin 文件
 *         7. 更新 pump 源到第一个转正后的 pbin，状态切换为 HIST_PUMP_NEW
 *         若校验失败，保留 fpbin.idx 以便下次恢复时重试转正。
 */
static void promote_fpbin_to_pbin(AppContext *ctx) {
    /* 1. Flush any remaining memory entries to current fpbin slice */
    if (ctx->fpbin_slice_file && ctx->fpbin_count > 0) {
        for (size_t i = 0; i < ctx->fpbin_count; i++) {
            write_pbin_record(ctx->fpbin_slice_file, ctx->fpbin_entries[i], &ctx->fpbin_stats[i]);
        }
        ctx->fpbin_line_count += ctx->fpbin_count;
        fpbin_clear_mem(ctx);
    }

    /* 2. Seal the last active fpbin slice with Footer */
    if (ctx->fpbin_slice_file) {
        write_pbin_footer(ctx->fpbin_slice_file, ctx->fpbin_line_count);
        fclose(ctx->fpbin_slice_file);
        ctx->fpbin_slice_file = NULL;
    }

    /* If no fpbin files were ever created but we have memory-only data,
       create a single fpbin slice file first so rename works uniformly. */
    if (ctx->fpbin_write_slice_index == 0 && ctx->fpbin_line_count == 0 && ctx->fpbin_count > 0) {
        /* This branch handles the case where fpbin never hit watermark
           and no slice file was opened. Create slice 0 now. */
        fpbin_open_slice(ctx);
        for (size_t i = 0; i < ctx->fpbin_count; i++) {
            write_pbin_record(ctx->fpbin_slice_file, ctx->fpbin_entries[i], &ctx->fpbin_stats[i]);
        }
        ctx->fpbin_line_count = ctx->fpbin_count;
        fpbin_clear_mem(ctx);
        write_pbin_footer(ctx->fpbin_slice_file, ctx->fpbin_line_count);
        fclose(ctx->fpbin_slice_file);
        ctx->fpbin_slice_file = NULL;
    }

    /* 3. Rename all fpbin slices to pbin */
    unsigned long pbin_start_idx = find_max_pbin_index(&ctx->cfg) + 1;
    unsigned long fpbin_total = ctx->fpbin_write_slice_index + 1;
    for (unsigned long i = 0; i < fpbin_total; i++) {
        char *src = get_fpbin_slice_filename(ctx->cfg.progress_base, i);
        char *dst = get_slice_filename(ctx->cfg.progress_base, pbin_start_idx + i);
        if (rename(src, dst) != 0) {
            log_error("fpbin 转正 rename 失败: %s -> %s", src, dst);
        }
        free(src);
        free(dst);
    }

    /* 4. Verify all promoted pbin slices */
    bool all_ok = true;
    for (unsigned long i = 0; i < fpbin_total; i++) {
        char *path = get_slice_filename(ctx->cfg.progress_base, pbin_start_idx + i);
        PbinFooter f;
        if (!read_pbin_footer(path, &f)) {
            log_error("转正后 pbin Footer 校验失败: %s", path);
            all_ok = false;
        }
        free(path);
    }
    if (!all_ok) {
        log_fatal("fpbin 转正校验未通过，建议手动清理后重试");
        /* 不删除 fpbin.idx，以便下次恢复时重试转正 */
        ctx->hist_pump_state = HIST_PUMP_DONE;
        return;
    }

    /* 5. Cleanup fpbin artifacts */
    char *fpbin_idx = get_fpbin_index_filename(ctx->cfg.progress_base);
    unlink(fpbin_idx);
    free(fpbin_idx);
    for (unsigned long i = 0; i < fpbin_total + 10; i++) {
        char *src = get_fpbin_slice_filename(ctx->cfg.progress_base, i);
        unlink(src);
        free(src);
    }

    /* 6. Update pump source to first promoted pbin */
    if (ctx->hist_pump_fp) fclose(ctx->hist_pump_fp);
    char *first_pbin = get_slice_filename(ctx->cfg.progress_base, pbin_start_idx);
    ctx->hist_pump_fp = fopen(first_pbin, "rb");
    free(first_pbin);
    ctx->hist_pump_slice_idx = pbin_start_idx;
    ctx->hist_pump_line_no = 0;

    /* 7. Reset pbin write state to after promoted slices */
    ctx->state.write_slice_index = pbin_start_idx + fpbin_total;
    ctx->state.line_count = 0;
    if (ctx->state.write_slice_file) {
        fclose(ctx->state.write_slice_file);
        ctx->state.write_slice_file = NULL;
    }

    ctx->hist_pump_state = HIST_PUMP_NEW;
}

/**
 * @brief  当前 pbin 分片消费完毕时的回调
 * @param  ctx  AppContext*  应用上下文指针，不能为空
 * @return void
 *
 * @note   关闭当前分片，尝试打开下一个散落分片；若无更多散落分片则检查 fpbin；
 *         若 fpbin 存在则触发转正流程；否则标记泵送完成（HIST_PUMP_DONE）。
 */
static void on_pbin_slice_consumed(AppContext *ctx) {
    if (ctx->hist_pump_fp) {
        fclose(ctx->hist_pump_fp);
        ctx->hist_pump_fp = NULL;
    }

    /* Try to open next scattered slice, skipping gaps (archived slices) */
    int missing = 0;
    while (missing <= 50) {
        ctx->hist_pump_slice_idx++;
        char *next_path = get_slice_filename(ctx->cfg.progress_base, ctx->hist_pump_slice_idx);
        ctx->hist_pump_fp = fopen(next_path, "rb");
        free(next_path);
        if (ctx->hist_pump_fp) {
            ctx->hist_pump_line_no = 0;
            return;  /* Continue pumping next slice */
        }
        missing++;
    }

    /* No more scattered slices. Check fpbin. */
    if (ctx->fpbin_count > 0 || ctx->fpbin_slice_file) {
        promote_fpbin_to_pbin(ctx);
        return;
    }

    /* Nothing left to pump */
    ctx->hist_pump_state = HIST_PUMP_DONE;
}

/**
 * @brief  从当前 pbin 文件读取下一条记录
 * @param  fp          FILE*              已打开的 pbin 文件指针，不能为空
 * @param  out_path    char**             输出路径字符串指针的指针，成功时指向新分配的字符串，调用方负责 free
 * @param  out_st      struct stat*       输出 stat 结构体指针，不能为空
 * @param  out_d_type  unsigned char*     输出文件类型指针，不能为空
 * @return bool  返回 true 表示读取成功；false 表示 EOF 或格式错误
 *
 * @note   对 path_len 做防御性校验（> MAX_PATH_LENGTH 则视为损坏数据）。
 *         根据 d_type 填充 stat::st_mode 中的文件类型位。
 */
static bool read_next_pbin_record(FILE *fp, char **out_path, struct stat *out_st, unsigned char *out_d_type) {
    size_t path_len;
    if (fread(&path_len, sizeof(size_t), 1, fp) != 1) return false;

    /* 防御性校验：防止读取到 Footer magic 或损坏数据 */
    if (path_len > MAX_PATH_LENGTH) {
        return false;
    }

    char *path = malloc(path_len + 1);
    if (!path) return false;
    if (fread(path, 1, path_len, fp) != path_len) { free(path); return false; }
    path[path_len] = '\0';

    dev_t dev; ino_t ino; time_t mtime; unsigned char d_type;
    if (fread(&dev, sizeof(dev_t), 1, fp) != 1) { free(path); return false; }
    if (fread(&ino, sizeof(ino_t), 1, fp) != 1) { free(path); return false; }
    if (fread(&mtime, sizeof(time_t), 1, fp) != 1) { free(path); return false; }
    if (fread(&d_type, sizeof(unsigned char), 1, fp) != 1) { free(path); return false; }

    memset(out_st, 0, sizeof(*out_st));
    out_st->st_dev = dev;
    out_st->st_ino = ino;
    out_st->st_mtime = mtime;
    out_st->st_mode = (d_type == DT_DIR) ? S_IFDIR :
                      (d_type == DT_REG) ? S_IFREG :
                      (d_type == DT_LNK) ? S_IFLNK :
                      (d_type == DT_CHR) ? S_IFCHR :
                      (d_type == DT_BLK) ? S_IFBLK :
                      (d_type == DT_FIFO) ? S_IFIFO :
                      (d_type == DT_SOCK) ? S_IFSOCK : 0;
    *out_path = path;
    *out_d_type = d_type;
    return true;
}

/**
 * @brief  从历史 pbin 分片中泵送一批目录给 Worker
 * @param  ctx        AppContext*  应用上下文指针，不能为空
 * @param  batch_size int          每批发送的目录数量，取值范围: > 0
 * @return void
 *
 * @note   从 hist_pump_fp 顺序读取记录，仅对 DT_DIR 类型的条目创建扫描任务并发送给 Worker。
 *         每批最多发送 batch_size 个目录。读取的目录会重新计算指纹并插入 visited_set 避免重复输出。
 *         当当前分片读完后自动调用 on_pbin_slice_consumed 切换到下一片或结束泵送。
 */
void pump_pbin_batch(AppContext *ctx, int batch_size) {
    if (!ctx->hist_pump_fp) return;

    int sent = 0;
    while (sent < batch_size) {
        char *path = NULL;
        struct stat st;
        unsigned char d_type;

        if (!read_next_pbin_record(ctx->hist_pump_fp, &path, &st, &d_type)) {
            on_pbin_slice_consumed(ctx);
            return;
        }

        ctx->hist_pump_line_no++;

        /* Load into visited_set to avoid duplicate output */
        uint8_t fp_all[FP_SIZE];
        fp_compute(path, st.st_dev, st.st_ino, fp_all);
        fp_set_insert(ctx->visited_set, fp_all);

        if (d_type == DT_DIR) {
            atomic_fetch_add(&ctx->pending_tasks, 1);
            uint32_t plen = (uint32_t)strlen(path);
            static int next_wid = 0;
            int wid = next_wid % ctx->worker_pool->num_workers;
            next_wid++;
            WorkerSlot *slot = &ctx->worker_pool->slots[wid];
            slot->current_dev = st.st_dev;
            safe_strcpy(slot->current_path, path, sizeof(slot->current_path));
            ipc_send(slot->fd_in, IPC_MSG_SCAN, path, plen);
            sent++;
        }
        free(path);
    }
}

/**
 * @brief  恢复模式：从归档和散落 pbin 加载进度并设置泵送状态
 * @param  cfg  const Config*  全局配置指针，不能为空
 * @param  ctx  AppContext*     应用上下文指针，不能为空
 * @return int  返回 0 表示恢复成功（或无需恢复）
 *
 * @note   恢复流程：
 *         1. 重置 fpbin 和 pump 状态
 *         2. 加载统一索引文件（idx）
 *         3. 统计归档块数和散落分片数
 *         4. 加载归档文件内容到 visited_set
 *         5. 若无索引且历史块数超过 1，执行全量重扫；否则加载散落分片
 *         6. 对有索引的情况，逐个加载散落 pbin 分片：
 *            - 已完成的旧分片（< write_slice_index）：完整解析
 *            - 活跃分片（== write_slice_index）：仅解析 line_count 行
 *            - Footer 有效时删除残留草稿 idx（"钢印清晰则烧草稿"）
 *         7. 处理残留 fpbin（上次转正中断）：重新执行 promote_fpbin_to_pbin
 *         8. 打开当前活跃分片，跳过已处理的 line_count 行，设置 pump 状态
 */
int restore_progress(const Config *cfg, AppContext *ctx) {
    /* 1. Reset fpbin state (keep residual files for potential re-promotion) */
    fpbin_clear_mem(ctx);
    if (ctx->fpbin_slice_file) {
        fclose(ctx->fpbin_slice_file);
        ctx->fpbin_slice_file = NULL;
    }
    ctx->fpbin_write_slice_index = 0;
    ctx->fpbin_line_count = 0;
    ctx->hist_pump_state = HIST_PUMP_DONE;
    ctx->hist_pump_fp = NULL;
    ctx->hist_pump_slice_idx = 0;
    ctx->hist_pump_line_no = 0;

    bool has_idx = load_progress_index(cfg, &ctx->state);
    unsigned long pbin_count  = count_pbin_slices(cfg);
    unsigned long archive_blk = count_archive_blocks(cfg);
    unsigned long total_blocks = pbin_count + archive_blk;

    /* 2. Load archive (completed slices) into visited_set */
    iterate_archive(cfg, ctx, ctx->visited_set, NULL, NULL);

    if (!has_idx) {
        if (total_blocks > 1) {
            verbose_printf(cfg, 1,
                "无索引文件且历史块数 %lu (archive=%lu, pbin=%lu) 超过一个，"
                "执行全量重扫...\n", total_blocks, archive_blk, pbin_count);
            ctx->state.write_slice_index = 0;
            ctx->state.line_count = 0;
            ctx->state.process_slice_index = 0;
            ctx->state.output_slice_num = 0;
            ctx->state.output_line_count = 0;
            /* Open first scattered slice for pumping */
            char *first_slice = get_slice_filename(cfg->progress_base, 0);
            ctx->hist_pump_fp = fopen(first_slice, "rb");
            free(first_slice);
            if (ctx->hist_pump_fp) {
                ctx->hist_pump_state = HIST_PUMP_OLD;
                ctx->hist_pump_slice_idx = 0;
                ctx->hist_pump_line_no = 0;
            }
            return 0;
        }
        verbose_printf(cfg, 1, "无索引文件，历史块数 %lu，尝试恢复...\n", total_blocks);
        ctx->state.write_slice_index = 0;
        ctx->state.line_count = 0;
        ctx->state.process_slice_index = 0;
        ctx->state.output_slice_num = 0;
        ctx->state.output_line_count = 0;
        /* Single block: load scattered slices and done */
        iterate_pbin_slices(cfg, &ctx->state, ctx->visited_set, NULL, NULL);
        return 0;
    }

    verbose_printf(cfg, 1, "开始断点恢复 (slice=%lu, line=%lu)...\n",
                   ctx->state.write_slice_index, ctx->state.line_count);

    /* 3. Load scattered pbin slices with Footer-first recovery */
    int consecutive_missing = 0;
    for (unsigned long s_idx = 0; ; ++s_idx) {
        char *slice_path = get_slice_filename(cfg->progress_base, s_idx);
        FILE *slice_fp = fopen(slice_path, "rb");
        if (!slice_fp) {
            free(slice_path);
            consecutive_missing++;
            if (consecutive_missing > 50 && s_idx > ctx->state.write_slice_index) break;
            continue;
        }
        consecutive_missing = 0;
        fseek(slice_fp, 0, SEEK_END);
        long fsize = ftell(slice_fp);
        fseek(slice_fp, 0, SEEK_SET);
        if (fsize > 0) {
            unsigned char *buf = safe_malloc(fsize);
            fread(buf, 1, fsize, slice_fp);

            long data_size = fsize;
            uint64_t row_count = 0;
            bool footer_ok = false;
            if (fsize >= (long)sizeof(PbinFooter)) {
                PbinFooter *f = (PbinFooter *)(buf + fsize - sizeof(PbinFooter));
                if (verify_pbin_footer(f)) {
                    data_size = fsize - (long)sizeof(PbinFooter);
                    row_count = f->row_count;
                    footer_ok = true;
                    /* 钢印清晰则烧草稿: 删除残留按分片 idx */
                    char *per_idx = get_per_slice_index_filename(cfg->progress_base, s_idx);
                    unlink(per_idx);
                    free(per_idx);
                }
            }
            if (!footer_ok) {
                /* Fallback: 读取按分片 idx 或统一 idx */
                row_count = get_slice_row_count(cfg, s_idx);
            }

            if (s_idx < ctx->state.write_slice_index) {
                /* 已完成分片：解析 row_count 行 */
                parse_pbin_buffer(buf, data_size, row_count, ctx->visited_set, NULL, NULL);
            } else if (s_idx == ctx->state.write_slice_index) {
                /* 活跃分片：只解析已处理的 line_count 行 */
                parse_pbin_buffer(buf, data_size, ctx->state.line_count, ctx->visited_set, NULL, NULL);
            }
            free(buf);
        }
        fclose(slice_fp);
        free(slice_path);
    }

    /* 4. Handle residual fpbin (interrupted promotion) */
    char *fpbin_idx_path = get_fpbin_index_filename(cfg->progress_base);
    bool has_fpbin_idx = (access(fpbin_idx_path, F_OK) == 0);
    bool has_fpbin_slice = false;
    for (unsigned long i = 0; i < 1000; i++) {
        char *fp = get_fpbin_slice_filename(cfg->progress_base, i);
        if (access(fp, F_OK) == 0) { has_fpbin_slice = true; free(fp); break; }
        free(fp);
    }
    if (has_fpbin_idx && has_fpbin_slice) {
        verbose_printf(cfg, 1, "检测到残留 fpbin，执行转正恢复...\n");
        FILE *fidx = fopen(fpbin_idx_path, "r");
        if (fidx) {
            unsigned long fsi = 0, flc = 0;
            if (fscanf(fidx, "%lu %lu", &fsi, &flc) == 2) {
                ctx->fpbin_write_slice_index = fsi;
                ctx->fpbin_line_count = flc;
            }
            fclose(fidx);
        }
        promote_fpbin_to_pbin(ctx);
        free(fpbin_idx_path);
        verbose_printf(cfg, 1, "fpbin 转正恢复完成\n");
        return 0;
    }
    /* 清理不匹配的残留 fpbin 文件 */
    if (!has_fpbin_idx || !has_fpbin_slice) {
        for (unsigned long i = 0; i < 1000; i++) {
            char *fp = get_fpbin_slice_filename(cfg->progress_base, i);
            unlink(fp);
            free(fp);
        }
        unlink(fpbin_idx_path);
    }
    free(fpbin_idx_path);

    /* 5. Open current pbin slice for pumping (skip processed lines) */
    char *cur_slice = get_slice_filename(cfg->progress_base, ctx->state.write_slice_index);
    ctx->hist_pump_fp = fopen(cur_slice, "rb");
    free(cur_slice);
    if (ctx->hist_pump_fp) {
        /* Skip already-processed lines */
        for (unsigned long i = 0; i < ctx->state.line_count; i++) {
            char *path = NULL;
            struct stat st;
            unsigned char d_type;
            if (!read_next_pbin_record(ctx->hist_pump_fp, &path, &st, &d_type)) break;
            free(path);
        }

        /* 如果已到达文件末尾，说明该分片已完全处理，无需 pumping */
        int c = fgetc(ctx->hist_pump_fp);
        if (c == EOF) {
            fclose(ctx->hist_pump_fp);
            ctx->hist_pump_fp = NULL;
            ctx->hist_pump_state = HIST_PUMP_DONE;
        } else {
            ungetc(c, ctx->hist_pump_fp);
            ctx->hist_pump_state = HIST_PUMP_OLD;
            ctx->hist_pump_slice_idx = ctx->state.write_slice_index;
            ctx->hist_pump_line_no = ctx->state.line_count;
        }
    }

    /* Fallback: if current slice is empty/missing, try pumping from slice 0
     * to replay all scattered slices (needed after abnormal termination) */
    if (ctx->hist_pump_state == HIST_PUMP_DONE) {
        char *first_slice = get_slice_filename(cfg->progress_base, 0);
        ctx->hist_pump_fp = fopen(first_slice, "rb");
        free(first_slice);
        if (ctx->hist_pump_fp) {
            ctx->hist_pump_state = HIST_PUMP_OLD;
            ctx->hist_pump_slice_idx = 0;
            ctx->hist_pump_line_no = 0;
        }
    }

    verbose_printf(cfg, 1, "进度加载完成\n");
    return 0;
}

/**
 * @brief  半增量模式：将历史索引加载到内存中的 reference_set/map
 * @param  cfg  const Config*  全局配置指针，不能为空
 * @param  ctx  AppContext*     应用上下文指针，不能为空
 * @return void
 *
 * @note   遍历归档文件和散落 pbin 分片，将指纹插入 reference_set，
 *         将 (mtime, d_type) 插入 reference_map。
 *         用于支撑半增量扫描的 blind-trust 机制。
 */
void restore_progress_to_memory(const Config *cfg, AppContext *ctx) {
    verbose_printf(cfg, 1, "开始加载半增量索引...\n");
    iterate_archive(cfg, ctx, NULL, ctx->reference_set, ctx->reference_map);
    iterate_pbin_slices(cfg, &ctx->state, NULL, ctx->reference_set, ctx->reference_map);
    verbose_printf(cfg, 1, "历史索引加载完成\n");
}

/* ================================================================
 * 其他辅助
 * ================================================================ */

/**
 * @brief  将当前会话配置快照保存到磁盘
 * @param  cfg  const Config*  全局配置指针，不能为空
 * @return void
 *
 * @note   写入 {base}.config 文件，包含：目标路径、输出文件、归档策略、CSV 模式、启动时间等。
 *         --clean 模式不创建任何进度文件。
 */
void save_config_to_disk(const Config* cfg) {
    if (!cfg->progress_base) return;
    if (cfg->clean) return; /* --clean mode should not leave any progress files */
    char config_path[1024];
    snprintf(config_path, sizeof(config_path), "%s.config", cfg->progress_base);
    FILE *fp = fopen(config_path, "w");
    if (!fp) return;
    fprintf(fp, "path=%s\n", cfg->target_path);
    if (cfg->output_file) fprintf(fp, "output=%s\n", cfg->output_file);
    if (cfg->output_split_dir) fprintf(fp, "output_split=%s\n", cfg->output_split_dir);
    fprintf(fp, "start_time=%ld\n", time(NULL));
    fprintf(fp, "archive=%d\n", cfg->archive);
    fprintf(fp, "clean=%d\n", cfg->clean);
    fprintf(fp, "csv=%d\n", cfg->csv);
    fprintf(fp, "status=Running\n");
    fclose(fp);
}

/**
 * @brief  任务结束时的进度收尾处理
 * @param  cfg    const Config*   全局配置指针，不能为空
 * @param  state  RuntimeState*   运行时状态指针，不能为空
 * @return void
 *
 * @note   非 --clean 模式：
 *         1. 调用 finalize_archive 封口活跃分片并归档
 *         2. 原子更新统一索引
 *         3. 追加状态行到 .config（Success/Incomplete + 结束时间）
 *         --clean 模式：
 *         关闭并删除活跃分片文件，不保留任何进度记录。
 */
void finalize_progress(const Config *cfg, RuntimeState *state) {
    if (!cfg->clean) {
        finalize_archive(cfg, state);
        /* Ensure index is written so resume can locate the cursor */
        atomic_update_index(cfg, state);
        if (cfg->progress_base) {
            char config_path[1024];
            snprintf(config_path, sizeof(config_path), "%s.config", cfg->progress_base);
            FILE *fp = fopen(config_path, "a");
            if (fp) {
                if (state->has_error) {
                    fprintf(fp, "status=Incomplete\n");
                    fprintf(fp, "error=DeviceMeltdown\n");
                } else {
                    fprintf(fp, "status=Success\n");
                }
                fprintf(fp, "end_time=%ld\n", time(NULL));
                fclose(fp);
            }
        }
    } else {
        /* --clean mode: do not create any new progress files */
        if (state->write_slice_file) {
            fclose(state->write_slice_file);
            state->write_slice_file = NULL;
            char *src_path = get_slice_filename(cfg->progress_base, state->write_slice_index);
            unlink(src_path);
            free(src_path);
        }
    }
}

/**
 * @brief  清理所有进度文件（--clean 或 --runone 时调用）
 * @param  cfg    const Config*   全局配置指针，不能为空
 * @param  state  RuntimeState*   运行时状态指针，不能为空
 * @return void
 *
 * @note   删除：统一索引、所有分片文件、按分片草稿 idx、归档文件、spbin、
 *         错误日志、config、fpbin 索引和分片、以及兼容旧版本的 progress.fpbin。
 *         注意：仅删除到 write_slice_index + 200 为止的分片，保留可能更远的残留。
 */
void cleanup_progress(const Config *cfg, RuntimeState *state) {
    char *idx_path = get_index_filename(cfg->progress_base);
    unlink(idx_path);
    free(idx_path);

    /* Always clean up slice files on --clean; on --archive they were already archived */
    if (cfg->clean || cfg->archive) {
        for (unsigned long i = 0; i <= state->write_slice_index + 200; i++) {
            char *slice_path = get_slice_filename(cfg->progress_base, i);
            unlink(slice_path);
            free(slice_path);
            /* 同时清理按分片草稿 idx */
            char *per_idx = get_per_slice_index_filename(cfg->progress_base, i);
            unlink(per_idx);
            free(per_idx);
        }
    }

    char *arch_path = get_archive_filename(cfg->progress_base);
    if (cfg->clean) unlink(arch_path);
    free(arch_path);

    char *spbin_path = get_spbin_filename(cfg->progress_base);
    unlink(spbin_path);
    free(spbin_path);

    char error_log[1024];
    snprintf(error_log, sizeof(error_log), "%s.error.log", cfg->progress_base);
    unlink(error_log);

    if (cfg->clean) {
        char config_path[1024];
        snprintf(config_path, sizeof(config_path), "%s.config", cfg->progress_base);
        unlink(config_path);
    }

    /* 清理残留 fpbin（基于 progress_base） */
    char *fpbin_idx = get_fpbin_index_filename(cfg->progress_base);
    unlink(fpbin_idx);
    free(fpbin_idx);
    for (unsigned long i = 0; i < 1000; i++) {
        char *fp = get_fpbin_slice_filename(cfg->progress_base, i);
        unlink(fp);
        free(fp);
    }
    /* 兼容旧版本残留 */
    unlink("progress.fpbin");
}

/**
 * @brief  获取文件锁（防止多实例同时操作同一进度文件）
 * @param  cfg    const Config*   全局配置指针，不能为空
 * @param  state  RuntimeState*   运行时状态指针，不能为空
 * @return int  返回 0 表示加锁成功；返回 -1 表示失败（文件不存在或其他进程已持有锁）
 *
 * @note   仅在 continue_mode 下生效。锁文件为 {base}.lock，使用 flock(LOCK_EX | LOCK_NB)。
 *         成功后将 fd 和路径记录到 state 中，由 release_lock 释放。
 */
int acquire_lock(const Config *cfg, RuntimeState *state) {
    if (!cfg->continue_mode) return 0;
    char *lock_path = safe_malloc(strlen(cfg->progress_base) + 32);
    sprintf(lock_path, "%s.lock", cfg->progress_base);
    state->lock_file_path = lock_path;
    int fd = open(lock_path, O_RDWR | O_CREAT, 0666);
    if (fd == -1) return -1;
    if (flock(fd, LOCK_EX | LOCK_NB) == -1) { close(fd); return -1; }
    state->lock_fd = fd;
    return 0;
}

/**
 * @brief  释放文件锁并删除锁文件
 * @param  state  RuntimeState*  运行时状态指针，不能为空
 * @return void
 */
void release_lock(RuntimeState *state) {
    if (state->lock_fd != -1) { flock(state->lock_fd, LOCK_UN); close(state->lock_fd); state->lock_fd = -1; }
    if (state->lock_file_path) { unlink(state->lock_file_path); free(state->lock_file_path); state->lock_file_path = NULL; }
}

/* ================================================================
 * Spbin memory cache
 * ================================================================ */

/**
 * @brief  向 spbin 内存缓存追加一条记录
 * @param  ctx    AppContext*         应用上下文指针，不能为空
 * @param  entry  const SpbinEntry*   要追加的跳过记录指针，不能为空
 * @return void
 *
 * @note   当缓存满时自动扩容至 2 倍容量。内存缓存用于设备恢复时快速重入队，
 *         避免重复读取 spbin 磁盘文件。
 */
void spbin_append(AppContext *ctx, const SpbinEntry *entry) {
    if (ctx->spbin_count >= ctx->spbin_capacity) {
        size_t new_cap = ctx->spbin_capacity ? ctx->spbin_capacity * 2 : 64;
        SpbinEntry *new_arr = realloc(ctx->spbin_entries, new_cap * sizeof(SpbinEntry));
        if (!new_arr) return;
        ctx->spbin_entries = new_arr;
        ctx->spbin_capacity = new_cap;
    }
    ctx->spbin_entries[ctx->spbin_count] = *entry;
    ctx->spbin_count++;
}

/**
 * @brief  设备恢复后，将 spbin 中该设备的积压路径重新入队扫描
 * @param  ctx  AppContext*  应用上下文指针，不能为空
 * @param  dev  dev_t        已恢复的设备号
 * @return void
 *
 * @note   遍历 spbin_entries 数组，找到匹配 dev 且状态非 CONDEMNED 的条目，
 *         增加 pending_tasks 并通过 ipc_send 发送 IPC_MSG_SCAN 给 Worker。
 *         使用 next_requeue_worker 轮询选择 Worker，实现负载均衡。
 */
void spbin_requeue_recovered(AppContext *ctx, dev_t dev) {
    for (size_t i = 0; i < ctx->spbin_count; i++) {
        if (ctx->spbin_entries[i].dev == dev && ctx->spbin_entries[i].s_status != SP_STATUS_CONDEMNED) {
            atomic_fetch_add(&ctx->pending_tasks, 1);
            uint32_t plen = (uint32_t)strlen(ctx->spbin_entries[i].path);
            int wid = ctx->next_requeue_worker % ctx->worker_pool->num_workers;
            WorkerSlot *slot = &ctx->worker_pool->slots[wid];
            slot->current_dev = ctx->spbin_entries[i].dev;
            safe_strcpy(slot->current_path, ctx->spbin_entries[i].path, sizeof(slot->current_path));
            ctx->next_requeue_worker++;
            ipc_send(ctx->worker_pool->slots[wid].fd_in, IPC_MSG_SCAN,
                     ctx->spbin_entries[i].path, plen);
        }
    }
}

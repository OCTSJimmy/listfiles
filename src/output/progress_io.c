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
#include "msg_format.h"
#include "msg_queue.h"
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
void write_pbin_record(FILE *fp, const char *path, const struct stat *info) {
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
void fpbin_open_slice(AppContext *ctx) {
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

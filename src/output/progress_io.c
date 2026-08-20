/**
 * @file progress_io.c
 * @brief pbin 分片 I/O、dpbin 完成日志与 fpbin 临时缓存
 *
 * 核心设计哲学：
 * - 同构分片：pbin、fpbin、dpbin 采用完全相同的物理格式
 * - 页脚自描述：已封口分片末尾自带 Footer（magic + row_count + crc），无需外部索引
 * - 崩溃恢复：Footer 优先，dpbin 提供差分集合用于续传
 *
 * 进度文件格式（以 --progress-file=task1 为例）：
 * - task1_000000.pbin  已封口的已完成记录分片
 * - task1.dpbin_000000 本次会话的目录完成日志（临时，正常结束后删除）
 * - task1.dfpbin_000XXX HIST_PUMP_OLD 阶段的目录完成日志（v15.6.0，fpbin 原子对）
 * - task1.spbin        跳过记录（熔断设备上的目录）
 * - task1.fpbin_000XXX 恢复期间隔离新发现子目录的临时分片
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

/* ================================================================
 * pbin / spbin 写入
 * ================================================================ */

/**
 * @brief  向 pbin/fpbin 文件写入单条记录（v15.6.0 schema 2，设计 §0.2）
 * @param  fp    FILE*             已打开的可写文件指针，不能为空
 * @param  path  const char*       文件路径，不能为空
 * @param  info  const struct stat* 文件 stat 信息指针，允许为 NULL（此时写入全 0）
 * @return void
 *
 * @note   单条记录格式（schema_version=2）：
 *         [path_len:size_t][path][d_type:u8][mtime_sec:time_t][mtime_nsec:long]
 *         [size:off_t][uid:u32][gid:u32][mode:u32][dev:u64][ino:u64][flags:u32]
 *         - dev/ino 定宽为 u64（仅全量扫描/诊断用，盲信不用）；
 *         - atime 完全排除（NFS 不可信）；
 *         - flags 预留（盲信安全字段版本），当前恒写 0；
 *         - mode 存完整 st_mode（类型位 + 权限位），供盲信命中后原样复用。
 *         旧格式（schema 1）一律不解析——manifest 缺失或 pbin_schema_version!=2
 *         时续传/盲信被拒绝（exit 2）。
 */
void write_pbin_record(FILE *fp, const char *path, const struct stat *info) {
    size_t path_len = strlen(path);
    unsigned char d_type = info ? mode_to_dtype(info->st_mode) : DT_UNKNOWN;
    time_t mtime_sec  = info ? info->st_mtim.tv_sec : 0;
    long   mtime_nsec = info ? info->st_mtim.tv_nsec : 0;
    off_t  size       = info ? info->st_size : 0;
    uint32_t uid      = info ? (uint32_t)info->st_uid : 0;
    uint32_t gid      = info ? (uint32_t)info->st_gid : 0;
    uint32_t mode     = info ? (uint32_t)info->st_mode : 0;
    uint64_t dev      = info ? (uint64_t)info->st_dev : 0;
    uint64_t ino      = info ? (uint64_t)info->st_ino : 0;
    uint32_t flags    = 0; /* 预留，恒 0 */

    fwrite(&path_len, sizeof(size_t), 1, fp);
    fwrite(path, 1, path_len, fp);
    fwrite(&d_type, sizeof(unsigned char), 1, fp);
    fwrite(&mtime_sec, sizeof(time_t), 1, fp);
    fwrite(&mtime_nsec, sizeof(long), 1, fp);
    fwrite(&size, sizeof(off_t), 1, fp);
    fwrite(&uid, sizeof(uint32_t), 1, fp);
    fwrite(&gid, sizeof(uint32_t), 1, fp);
    fwrite(&mode, sizeof(uint32_t), 1, fp);
    fwrite(&dev, sizeof(uint64_t), 1, fp);
    fwrite(&ino, sizeof(uint64_t), 1, fp);
    fwrite(&flags, sizeof(uint32_t), 1, fp);
}

/**
 * @brief  记录单条已处理路径到当前活跃 pbin 分片
 * @param  cfg   const Config*       全局配置指针，不能为空
 * @param  state RuntimeState*       运行时状态指针，不能为空
 * @param  path  const char*         文件路径，不能为空
 * @param  info  const struct stat*  文件 stat 信息指针，允许为 NULL
 * @return void
 *
 * @note   若当前无活跃分片，自动创建新的 pbin 文件。
 *         当 line_count 达到 progress_slice_lines（默认 100000）时执行分片轮转：
 *         1. 写入 Footer 封口当前分片
 *         2. 调用 process_old_slice 处理旧分片（归档或删除）
 *         3. 创建新分片继续写入
 */
void record_path(const Config *cfg, RuntimeState *state, const char *path, const struct stat *info) {
    if (cfg->clean) return;  /* --clean 模式不保留任何进度文件 */
    if (!state->write_slice_file) {
        char *p = get_slice_filename(cfg->progress_base, state->write_slice_index);
        state->write_slice_file = fopen(p, "wb");
        free(p);
    }
    if (!state->write_slice_file) return;
    write_pbin_record(state->write_slice_file, path, info);
    state->line_count++;
    state->processed_count++;
    if (state->line_count >= cfg->progress_slice_lines) {
        /* rotate slice: 封口当前分片 */
        write_pbin_footer(state->write_slice_file, state->line_count);
        fclose(state->write_slice_file);
        state->write_slice_file = NULL;

        process_old_slice(cfg, state->write_slice_index);
        state->write_slice_index++;
        state->line_count = 0;
        char *p = get_slice_filename(cfg->progress_base, state->write_slice_index);
        state->write_slice_file = fopen(p, "wb");
        free(p);
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
    /* v15.6.0（§0.2 schema 2）：定长尾部长度 =
     * d_type(1) + mtime_sec + mtime_nsec + size + uid/gid/mode(3×u32) + dev/ino(2×u64) + flags(u32) */
    size_t entry_size = path_len + sizeof(unsigned char) + sizeof(time_t) + sizeof(long)
                      + sizeof(off_t) + 3 * sizeof(uint32_t) + 2 * sizeof(uint64_t) + sizeof(uint32_t);
    
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
 * @brief  向已打开的 spbin 文件写入单条记录（v15.6.0 新格式，P0-005）
 * @param  fp     FILE*             已打开的可写文件指针，不能为空
 * @param  entry  const SpbinEntry* 跳过记录条目指针，不能为空
 * @return void
 *
 * @note   记录格式：[path_len:u32][path][reason:u8][timestamp:time_t][device_key:64B]。
 *         device_key 暂填 st_dev 的十进制字符串（NUL 结尾，余量清零）——
 *         P1-003 将升级为 (fsid, server, export) 三元组。
 *         本函数不 fflush，由调用方决定刷盘时机。
 */
void spbin_file_write_entry(FILE *fp, const SpbinEntry *entry) {
    if (!fp || !entry || !entry->path) return;

    uint32_t path_len = (uint32_t)strlen(entry->path);
    char device_key[SPBIN_DEVICE_KEY_LEN] = {0};
    snprintf(device_key, sizeof(device_key), "%lu", (unsigned long)entry->dev);

    fwrite(&path_len, sizeof(uint32_t), 1, fp);
    fwrite(entry->path, 1, path_len, fp);
    fwrite(&entry->reason, sizeof(uint8_t), 1, fp);
    fwrite(&entry->timestamp, sizeof(time_t), 1, fp);
    fwrite(device_key, 1, SPBIN_DEVICE_KEY_LEN, fp);
}

/**
 * @brief  将跳过记录（spbin）追加到磁盘文件（append-only）
 * @param  progress_base  const char*        进度文件前缀，不能为空
 * @param  entry          const SpbinEntry*  跳过记录条目指针，不能为空
 * @return void
 *
 * @note   v15.6.0（P0-005）：替代死代码 record_skip。以追加模式（"ab"）打开
 *         {base}.spbin 并立即 fflush——跳过记录是崩溃恢复依据，不得滞留在
 *         stdio 缓冲中。append-only：运行期不随机改写，恢复条目由正常退出时
 *         的 spbin_compact 统一过滤。
 */
void spbin_file_append(const char *progress_base, const SpbinEntry *entry) {
    if (!progress_base || !entry) return;
    char *spbin_path = get_spbin_filename(progress_base);
    FILE *fp = fopen(spbin_path, "ab");
    free(spbin_path);
    if (!fp) return;

    spbin_file_write_entry(fp, entry);
    fflush(fp);
    fclose(fp);
}

/* ================================================================
 * 索引与游标
 * ================================================================ */

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

/**
 * @brief  将 fpbin 内存缓冲刷出到活跃分片并 fflush（v15.6.0，P0-003）
 * @param  ctx  AppContext*  应用上下文指针，不能为空
 * @return void
 *
 * @note   耐久性顺序要求：父目录"完成"写 dfpbin 之前，其新发现子目录必须已在
 *         fpbin 落盘（内存缓冲刷出 + fflush 活跃分片）。否则崩溃后父目录在
 *         dfpbin、子目录丢失 → 恢复时父目录被 completed_set 剪枝 → 子树漏扫。
 *         由 main_loop.c advance_task_barriers 在 dfpbin_append 前调用。
 */
void fpbin_flush(AppContext *ctx) {
    if (!ctx) return;
    if (ctx->fpbin_count > 0) {
        if (!ctx->fpbin_slice_file) {
            fpbin_open_slice(ctx);
        }
        if (ctx->fpbin_slice_file) {
            for (size_t i = 0; i < ctx->fpbin_count; i++) {
                write_pbin_record(ctx->fpbin_slice_file, ctx->fpbin_entries[i], &ctx->fpbin_stats[i]);
            }
            ctx->fpbin_line_count += ctx->fpbin_count;
        }
        for (size_t i = 0; i < ctx->fpbin_count; i++) {
            free(ctx->fpbin_entries[i]);
        }
        ctx->fpbin_count = 0;
        if (ctx->fpbin_slice_file && ctx->fpbin_line_count >= ctx->cfg.progress_slice_lines) {
            fpbin_rotate_slice(ctx);
        }
    }
    if (ctx->fpbin_slice_file) {
        fflush(ctx->fpbin_slice_file);
    }
}

/* ================================================================
 * dpbin 完成日志（本次会话临时，正常结束后删除）
 * 格式与 pbin 同构（v15.6.0 schema 2）：path | d_type | mtime_sec | mtime_nsec | size | uid | gid | mode | dev | ino | flags
 * ================================================================ */

/**
 * @brief  获取指定 dpbin 分片的文件路径
 * @param  base   const char*    进度文件基础名（--progress-file 的值），不能为空
 * @param  index  unsigned long  分片编号，取值范围: >= 0
 * @return char*  动态分配的字符串，包含完整分片路径；调用者负责 free。
 */
char *get_dpbin_slice_filename(const char *base, unsigned long index) {
    char *name = safe_malloc(strlen(base) + 32);
    snprintf(name, strlen(base) + 32, "%s.dpbin_%06lu", base, index);
    return name;
}

/**
 * @brief  打开或创建新的 dpbin 活跃分片
 * @param  ctx  AppContext*  应用上下文指针，不能为空
 * @return void
 */
void dpbin_open_slice(AppContext *ctx) {
    if (ctx->dpbin_slice_file) {
        fclose(ctx->dpbin_slice_file);
        ctx->dpbin_slice_file = NULL;
    }
    char *p = get_dpbin_slice_filename(ctx->cfg.progress_base, ctx->dpbin_write_slice_index);
    ctx->dpbin_slice_file = fopen(p, "wb");
    free(p);
    ctx->dpbin_line_count = 0;
}

/**
 * @brief  轮转 dpbin 分片（封口当前分片并创建新分片）
 * @param  ctx  AppContext*  应用上下文指针，不能为空
 * @return void
 */
static void dpbin_rotate_slice_internal(AppContext *ctx) {
    if (ctx->dpbin_slice_file) {
        write_pbin_footer(ctx->dpbin_slice_file, ctx->dpbin_line_count);
        fclose(ctx->dpbin_slice_file);
        ctx->dpbin_slice_file = NULL;
    }
    ctx->dpbin_write_slice_index++;
    dpbin_open_slice(ctx);
}

/**
 * @brief  向 dpbin 追加一条记录（目录完成时写入）
 * @param  ctx   AppContext*         应用上下文指针，不能为空
 * @param  path  const char*         目录路径，不能为空
 * @param  st    const struct stat*  目录 stat 信息指针，允许为 NULL
 * @return void
 *
 * @note   直接追加写入当前 dpbin 分片，无内存缓冲。
 *         当分片行数达到 progress_slice_lines 时自动轮转。
 */
void dpbin_append(AppContext *ctx, const char *path, const struct stat *st) {
    if (!ctx->dpbin_slice_file) {
        dpbin_open_slice(ctx);
    }
    if (!ctx->dpbin_slice_file) return;

    write_pbin_record(ctx->dpbin_slice_file, path, st);
    ctx->dpbin_line_count++;

    if (ctx->dpbin_line_count >= ctx->cfg.progress_slice_lines) {
        dpbin_rotate_slice_internal(ctx);
    }
}

/**
 * @brief  删除所有 dpbin 文件（正常扫描结束后调用）
 * @param  progress_base  const char*  进度文件基础名，不能为空
 * @return void
 *
 * @note   扫描目录中所有匹配 dpbin_*. 的文件并删除。
 *         若 dpbin 从未创建过（无文件），本函数为空操作。
 */
void dpbin_delete_all(const char *progress_base) {
    char *dir = strdup(progress_base);
    char *last_slash = strrchr(dir, '/');
    if (last_slash) {
        *last_slash = '\0';
    } else {
        free(dir);
        dir = strdup(".");
    }

    DIR *d = opendir(dir);
    if (!d) {
        free(dir);
        return;
    }

    const char *base_name = last_slash ? last_slash + 1 : progress_base;
    size_t prefix_len = strlen(base_name) + strlen(".dpbin_");
    char *prefix = safe_malloc(prefix_len + 1);
    snprintf(prefix, prefix_len + 1, "%s.dpbin_", base_name);

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (strncmp(entry->d_name, prefix, strlen(prefix)) == 0) {
            char *full = safe_malloc(strlen(dir) + strlen(entry->d_name) + 2);
            snprintf(full, strlen(dir) + strlen(entry->d_name) + 2, "%s/%s", dir, entry->d_name);
            unlink(full);
            free(full);
        }
    }

    free(prefix);
    closedir(d);
    free(dir);
}

/* ================================================================
 * v15.6.0（P0-003）：dfpbin 完成日志（HIST_PUMP_OLD 阶段，fpbin 原子对）
 * 格式与 dpbin/pbin 同构（v15.6.0 schema 2）：path | d_type | mtime_sec | mtime_nsec | size | uid | gid | mode | dev | ino | flags + Footer + 轮转
 * ================================================================ */

/**
 * @brief  获取指定 dfpbin 分片的文件路径
 * @param  base   const char*    进度文件基础名（--progress-file 的值），不能为空
 * @param  index  unsigned long  分片编号，取值范围: >= 0
 * @return char*  动态分配的字符串，包含完整分片路径；调用者负责 free。
 */
char *get_dfpbin_slice_filename(const char *base, unsigned long index) {
    char *name = safe_malloc(strlen(base) + 32);
    snprintf(name, strlen(base) + 32, "%s.dfpbin_%06lu", base, index);
    return name;
}

/**
 * @brief  打开或创建新的 dfpbin 活跃分片
 * @param  ctx  AppContext*  应用上下文指针，不能为空
 * @return void
 */
static void dfpbin_open_slice(AppContext *ctx) {
    if (ctx->dfpbin_slice_file) {
        fclose(ctx->dfpbin_slice_file);
        ctx->dfpbin_slice_file = NULL;
    }
    char *p = get_dfpbin_slice_filename(ctx->cfg.progress_base, ctx->dfpbin_write_slice_index);
    ctx->dfpbin_slice_file = fopen(p, "wb");
    free(p);
    ctx->dfpbin_line_count = 0;
}

/**
 * @brief  轮转 dfpbin 分片（封口当前分片并创建新分片）
 * @param  ctx  AppContext*  应用上下文指针，不能为空
 * @return void
 */
static void dfpbin_rotate_slice_internal(AppContext *ctx) {
    if (ctx->dfpbin_slice_file) {
        write_pbin_footer(ctx->dfpbin_slice_file, ctx->dfpbin_line_count);
        fclose(ctx->dfpbin_slice_file);
        ctx->dfpbin_slice_file = NULL;
    }
    ctx->dfpbin_write_slice_index++;
    dfpbin_open_slice(ctx);
}

/**
 * @brief  向 dfpbin 追加一条记录（HIST_PUMP_OLD 阶段目录完成时写入）
 * @param  ctx   AppContext*         应用上下文指针，不能为空
 * @param  path  const char*         目录路径，不能为空
 * @param  st    const struct stat*  目录 stat 信息指针，允许为 NULL
 * @return void
 *
 * @note   复用 dpbin 的写入/轮转逻辑。调用前必须先 fpbin_flush（见
 *         main_loop.c advance_task_barriers），保证子目录先于父目录落盘。
 *         当分片行数达到 progress_slice_lines 时自动轮转。
 */
void dfpbin_append(AppContext *ctx, const char *path, const struct stat *st) {
    if (!ctx->dfpbin_slice_file) {
        dfpbin_open_slice(ctx);
    }
    if (!ctx->dfpbin_slice_file) return;

    write_pbin_record(ctx->dfpbin_slice_file, path, st);
    ctx->dfpbin_line_count++;

    if (ctx->dfpbin_line_count >= ctx->cfg.progress_slice_lines) {
        dfpbin_rotate_slice_internal(ctx);
    }
}

/**
 * @brief  删除所有 dfpbin 文件（整对抛弃 / --runone 清理时调用）
 * @param  progress_base  const char*  进度文件基础名，不能为空
 * @return void
 *
 * @note   扫描目录中所有匹配 dfpbin_*. 的文件并删除。
 *         若 dfpbin 从未创建过（无文件），本函数为空操作。
 */
void dfpbin_delete_all(const char *progress_base) {
    char *dir = strdup(progress_base);
    char *last_slash = strrchr(dir, '/');
    if (last_slash) {
        *last_slash = '\0';
    } else {
        free(dir);
        dir = strdup(".");
    }

    DIR *d = opendir(dir);
    if (!d) {
        free(dir);
        return;
    }

    const char *base_name = last_slash ? last_slash + 1 : progress_base;
    size_t prefix_len = strlen(base_name) + strlen(".dfpbin_");
    char *prefix = safe_malloc(prefix_len + 1);
    snprintf(prefix, prefix_len + 1, "%s.dfpbin_", base_name);

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (strncmp(entry->d_name, prefix, strlen(prefix)) == 0) {
            char *full = safe_malloc(strlen(dir) + strlen(entry->d_name) + 2);
            snprintf(full, strlen(dir) + strlen(entry->d_name) + 2, "%s/%s", dir, entry->d_name);
            unlink(full);
            free(full);
        }
    }

    free(prefix);
    closedir(d);
    free(dir);
}

/**
 * @brief  修复截断的 pbin 分片（ salvage 有效行，截断后重新封口）
 * @param  path            const char*   分片文件路径，不能为空
 * @param  out_valid_rows  uint64_t*     输出参数，返回 salvage 后的有效行数
 * @return bool  返回 true 表示 salvage 成功（至少保留了一条记录并写入 Footer）；
 *               false 表示整个分片损坏或为空，建议直接删除。
 *
 * @note   从文件开头顺序解析记录，遇到第一条不完整记录时停止。
 *         截断到最后一条完整记录末尾，写入新的 Footer（row_count = 有效行数）。
 *         各字段大小与 write_pbin_record 严格对应。
 */
bool pbin_salvage_truncated(const char *path, uint64_t *out_valid_rows) {
    FILE *fp = fopen(path, "r+b");
    if (!fp) return false;

    uint64_t valid_offset = 0;
    uint64_t valid_rows = 0;
    char path_buf[MAX_PATH_LENGTH];

    while (1) {
        size_t path_len;
        if (fread(&path_len, sizeof(size_t), 1, fp) != 1) break;
        if (path_len == 0 || path_len >= MAX_PATH_LENGTH) break;

        if (fread(path_buf, 1, path_len, fp) != path_len) break;
        /* 可选：检查路径是否为合理字符串，但二进制数据可能恰好匹配，不强制校验 */

        /* v15.6.0（§0.2 schema 2）：定长尾部，与 write_pbin_record 严格对应 */
        unsigned char d_type;
        time_t mtime_sec;
        long mtime_nsec;
        off_t size;
        uint32_t uid, gid, mode;
        uint64_t dev, ino;
        uint32_t flags;
        if (fread(&d_type, sizeof(unsigned char), 1, fp) != 1) break;
        if (fread(&mtime_sec, sizeof(time_t), 1, fp) != 1) break;
        if (fread(&mtime_nsec, sizeof(long), 1, fp) != 1) break;
        if (fread(&size, sizeof(off_t), 1, fp) != 1) break;
        if (fread(&uid, sizeof(uint32_t), 1, fp) != 1) break;
        if (fread(&gid, sizeof(uint32_t), 1, fp) != 1) break;
        if (fread(&mode, sizeof(uint32_t), 1, fp) != 1) break;
        if (fread(&dev, sizeof(uint64_t), 1, fp) != 1) break;
        if (fread(&ino, sizeof(uint64_t), 1, fp) != 1) break;
        if (fread(&flags, sizeof(uint32_t), 1, fp) != 1) break;

        valid_offset = ftell(fp);
        valid_rows++;
    }

    if (valid_rows == 0) {
        fclose(fp);
        return false;
    }

    /* 截断到最后一条完整记录末尾 */
    if (ftruncate(fileno(fp), (off_t)valid_offset) != 0) {
        fclose(fp);
        return false;
    }
    fseek(fp, (long)valid_offset, SEEK_SET);

    /* 重新写入 Footer */
    bool ok = write_pbin_footer(fp, valid_rows);
    fclose(fp);

    if (ok && out_valid_rows) *out_valid_rows = valid_rows;
    return ok;
}

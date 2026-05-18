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
            
            /* v13.0.0: Send CMD_SCAN through cmd_queue instead of direct ipc_send */
            CmdScanPayload *scan = malloc(sizeof(CmdScanPayload));
            if (!scan) {
                log_warn("[SPBIN] malloc failed for CMD_SCAN, dropping %s", ctx->spbin_entries[i].path);
                atomic_fetch_sub(&ctx->pending_tasks, 1);
                lost_tasks_push(&ctx->lost_tasks, strdup(ctx->spbin_entries[i].path));
            } else {
                scan->path_len = plen;
                scan->dev = ctx->spbin_entries[i].dev;
                safe_strcpy(scan->path, ctx->spbin_entries[i].path, sizeof(scan->path));
                IpcThreadMsg msg = {
                    .type = CMD_SCAN,
                    .slot_id = wid,
                    .data = scan,
                    .data_len = sizeof(*scan)
                };
                if (!msg_queue_send(ctx->ipc_cmd_queues[wid], &msg)) {
                    free(scan);
                    log_warn("[SPBIN] cmd_queue full, dropping %s", ctx->spbin_entries[i].path);
                    atomic_fetch_sub(&ctx->pending_tasks, 1);
                    lost_tasks_push(&ctx->lost_tasks, strdup(ctx->spbin_entries[i].path));
                }
            }
        }
    }
}

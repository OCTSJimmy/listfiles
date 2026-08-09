/**
 * @file dispatch.c
 * @brief 任务调度分发、Worker 清理与 IPC send 辅助函数
 *
 * 负责将目录任务分发给空闲 Worker，处理 lost task 重发，
 * 以及清理死亡 Worker 的管道与状态。
 */
#define _GNU_SOURCE
#include "main_loop.h"
#include "utils.h"
#include "progress.h"
#include "worker_proc.h"
#include "log.h"
#include "msg_format.h"
#include "msg_queue.h"
#include "ipc_thread.h"
#include "dispatch_queue.h"
#include "circuit_breaker.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include <stdatomic.h>
#include <dirent.h>

/* ================================================================
 * IPC helper: send CMD_SCAN to IPC thread
 * ================================================================ */

bool send_scan_to_ipc(AppContext *ctx, int wid, const char *path, uint64_t dev) {
    CmdScanPayload *scan = malloc(sizeof(CmdScanPayload));
    if (!scan) return false;
    scan->path_len = (uint32_t)strlen(path);
    scan->dev = dev;
    safe_strcpy(scan->path, path, sizeof(scan->path));

    IpcThreadMsg msg = {
        .type = CMD_SCAN,
        .slot_id = wid,
        .data = scan,
        .data_len = sizeof(*scan)
    };

    if (!msg_queue_send(ctx->ipc_cmd_queues[wid], &msg)) {
        log_warn_v(202607030000UL, "[Dispatch] cmd_queue[%d] full, dropping %s", wid, path_log_mask(path));
        free(scan);
        return false;
    }
    return true;
}

/* ================================================================
 * IPC helper: send CMD_REPLACE to IPC thread
 * ================================================================ */

void send_replace_to_ipc(AppContext *ctx, int wid, int fd_cmd, int fd_data, int fd_ctrl, pid_t pid) {
    CmdReplacePayload *rep = malloc(sizeof(CmdReplacePayload));
    if (!rep) {
        log_error("[Replace] malloc failed for worker %d", wid);
        return;
    }
    rep->fd_cmd = fd_cmd;
    rep->fd_data = fd_data;
    rep->fd_ctrl = fd_ctrl;
    rep->pid = pid;

    IpcThreadMsg msg = {
        .type = CMD_REPLACE,
        .slot_id = wid,
        .data = rep,
        .data_len = sizeof(*rep)
    };

    if (!msg_queue_send(ctx->ipc_cmd_queues[wid], &msg)) {
        log_error("[Replace] cmd_queue[%d] full, REPLACE dropped", wid);
        free(rep);
    }
}

/* ================================================================
 * IPC helper: send CMD_STOP to IPC thread
 * ================================================================ */

void send_stop_to_ipc(AppContext *ctx, int wid) {
    IpcThreadMsg msg = {
        .type = CMD_STOP,
        .slot_id = wid,
        .data = NULL,
        .data_len = 0
    };
    msg_queue_send(ctx->ipc_cmd_queues[wid], &msg);
}

/* ================================================================
 * Worker dispatch helper: find next available IDLE worker
 * ================================================================ */

int dispatch_find_idle_worker(AppContext *ctx) {
    int num_workers = ctx->worker_pool->num_workers;
    int attempts = 0;
    while (attempts < num_workers) {
        int candidate = ctx->next_dispatch_worker % num_workers;
        ctx->next_dispatch_worker++;
        WorkerSlot *cand_slot = &ctx->worker_pool->slots[candidate];
        if (!atomic_load(&cand_slot->is_alive)) { attempts++; continue; }
        if (atomic_load(&cand_slot->state) != WORKER_STATE_IDLE) { attempts++; continue; }
        return candidate;
    }
    return -1;
}

/* ================================================================
 * Dispatch from queue (v15.5.0: Stage 4 consumes dispatch_queue)
 * ================================================================ */

void dispatch_from_queue(AppContext *ctx) {
    /* 短路：如果所有 Worker 都死了，直接返回 */
    bool any_alive = false;
    for (int i = 0; i < ctx->worker_pool->num_workers; i++) {
        if (atomic_load(&ctx->worker_pool->slots[i].is_alive)) {
            any_alive = true;
            break;
        }
    }
    if (!any_alive) return;

    DispatchTask task;
    while (dispatch_queue_pop(&ctx->dispatch_queue, &task)) {
        if (!task.path) continue;

        int wid = dispatch_find_idle_worker(ctx);
        if (wid < 0) {
            log_warn_v(202607030000UL, "[DispatchQueue] no IDLE worker available, requeue %s", path_log_mask(task.path));
            if (!dispatch_queue_push(&ctx->dispatch_queue, task.path, &task.st)) {
                free(task.path);
            }
            break; /* 停止继续尝试，等下一轮 */
        }

        /* v15.5.9: redispatch 退避检查——该 slot 若处于退避期，跳过 */
        if (ctx->redispatch_backoff_until[wid] > 0) {
            time_t now = time(NULL);
            if (now < ctx->redispatch_backoff_until[wid]) {
                log_debug_v(202607030000UL, "[DispatchQueue] worker %d in backoff until %ld (now=%ld), requeue %s",
                            wid, (long)ctx->redispatch_backoff_until[wid], (long)now, path_log_mask(task.path));
                if (!dispatch_queue_push(&ctx->dispatch_queue, task.path, &task.st)) {
                    free(task.path);
                }
                continue; /* 尝试下一个任务 */
            }
            /* 退避已过期，清除标记 */
            ctx->redispatch_backoff_until[wid] = 0;
        }

        WorkerSlot *slot = &ctx->worker_pool->slots[wid];
        atomic_store(&slot->state, WORKER_STATE_BUSY);

        if (!send_scan_to_ipc(ctx, wid, task.path, task.st.st_dev)) {
            atomic_store(&slot->state, WORKER_STATE_IDLE);
            if (!dispatch_queue_push(&ctx->dispatch_queue, task.path, &task.st)) {
                free(task.path);
            }
            continue;
        }
        /* v15.5.0: pending_tasks++ and dpbin_append only on successful dispatch */
        atomic_fetch_add(&ctx->pending_tasks, 1);
        if (task.st.st_dev != 0) {
            dpbin_append(ctx, task.path, &task.st);
        }
        log_debug_v(202605201600UL, "[DispatchQueue] dispatched %s to worker %d, pending_tasks=%ld", path_log_mask(task.path), wid, atomic_load(&ctx->pending_tasks));

        slot->current_dev = task.st.st_dev;
        safe_strcpy(slot->current_path, task.path, sizeof(slot->current_path));
        free(task.path);
    }
    dispatch_queue_compact(&ctx->dispatch_queue);
}

/* ================================================================
 * v15.5.3: Circuit breaker for DEV_TIMEOUT redispatch loop
 * ================================================================ */

/**
 * @brief  检查路径是否已被熔断（连续 DEV_TIMEOUT 超过阈值）
 * @param  ctx      AppContext*  应用上下文
 * @param  wid      int          Worker slot id
 * @param  path     const char*  要检查的路径
 * @return bool     true = 路径已熔断，不应再重试；false = 可以重试
 *
 * @note   同一个 Worker slot 上，如果连续 timeout 的路径相同且次数达到
 *         CIRCUIT_BREAKER_THRESHOLD，则熔断该路径，不再 redispatch。
 *         不同路径会重置计数器。熔断路径记录 WARN 日志。
 */
static bool circuit_breaker_check(AppContext *ctx, int wid, const char *path) {
    if (wid < 0 || wid >= 8) return false;

    if (strcmp(ctx->timeout_paths[wid], path) == 0) {
        ctx->timeout_counts[wid]++;
    } else {
        safe_strcpy(ctx->timeout_paths[wid], path, sizeof(ctx->timeout_paths[wid]));
        ctx->timeout_counts[wid] = 1;
    }

    if (ctx->timeout_counts[wid] >= CIRCUIT_BREAKER_THRESHOLD) {
        log_warn_v(202607030000UL, "[CircuitBreaker] Path timed out %d times, skipping: %s",
                   ctx->timeout_counts[wid], path_log_mask(path));
        circuit_breaker_record(ctx, "PATH_TIMEOUT", path, 0, ctx->timeout_counts[wid]);
        return true; /* 熔断：不再重试 */
    }
    return false; /* 未熔断：允许重试 */
}

/* ================================================================
 * Cleanup dead worker slot (v13.0.0: no epoll DEL, IPC thread handles fd)
 * ================================================================ */

void cleanup_dead_worker_slot(AppContext *ctx, int worker_id, bool redispatch_current) {
    if (!ctx || !ctx->worker_pool) return;
    if (worker_id < 0 || worker_id >= ctx->worker_pool->num_workers) return;
    WorkerSlot *slot = &ctx->worker_pool->slots[worker_id];
    if (!atomic_load(&slot->is_alive) && slot->pid == -1) return;
    if (atomic_flag_test_and_set(&slot->cleanup_done)) return;

    /* Drain fd_cmd_rd to count orphaned SCAN tasks */
    int orphaned = 0;
    if (slot->fd_cmd_rd >= 0) {
        orphaned = ipc_drain_and_count_tasks(slot->fd_cmd_rd);
        if (orphaned > 0) {
            log_debug_v(202605201600UL, "[Cleanup] Worker %d drained %d orphaned tasks from fd_cmd_rd", worker_id, orphaned);
        }
        close(slot->fd_cmd_rd);
        slot->fd_cmd_rd = -1;
    }

    /* Migrate backlog to dispatch_queue (stats unknown, st_dev==0 marks re-dispatch) */
    dispatch_queue_push_backlog(&ctx->dispatch_queue, slot->backlog_paths, NULL, slot->backlog_count);
    free(slot->backlog_paths);
    slot->backlog_paths = NULL;
    slot->backlog_count = 0;
    slot->backlog_capacity = 0;

    /* Close write-end fd_cmd (IPC thread already closed its copy, Master just notes it) */
    if (slot->fd_cmd >= 0) {
        slot->fd_cmd = -1;
    }

    /* fd_data and fd_ctrl are closed by IPC thread; Master just notes them */
    if (slot->fd_data >= 0) {
        slot->fd_data = -1;
    }
    if (slot->fd_ctrl >= 0) {
        slot->fd_ctrl = -1;
    }

    atomic_fetch_sub(&ctx->pending_tasks, 1 + orphaned);

    /* v15.5.3: Circuit breaker for DEV_TIMEOUT redispatch loop
     * v15.5.9: 增加指数退避——同一目录连续超时后，redispatch 前等待
     * 30s -> 120s -> 300s，给 NFS 大目录喘息时间 */
    if (redispatch_current && slot->current_path[0] != '\0') {
        bool tripped = circuit_breaker_check(ctx, worker_id, slot->current_path);
        if (!tripped) {
            /* 计算退避时间：基于已超时次数 */
            int backoff_sec = 0;
            if (ctx->timeout_counts[worker_id] == 1) backoff_sec = 30;
            else if (ctx->timeout_counts[worker_id] == 2) backoff_sec = 120;
            else if (ctx->timeout_counts[worker_id] >= 3) backoff_sec = 300;

            if (backoff_sec > 0) {
                ctx->redispatch_backoff_until[worker_id] = time(NULL) + backoff_sec;
                log_info_v(202607030000UL, "[CircuitBreaker] Path timeout count=%d, backoff %ds before redispatch: %s",
                           ctx->timeout_counts[worker_id], backoff_sec, path_log_mask(slot->current_path));
            } else {
                ctx->redispatch_backoff_until[worker_id] = 0;
            }

            char *dup = strdup(slot->current_path);
            if (!dispatch_queue_push(&ctx->dispatch_queue, dup, NULL)) {
                free(dup);
            }
        }
        /* If tripped: path is skipped, pending_tasks already decremented above */
    }

    if (atomic_load(&slot->is_alive)) {
        atomic_store(&slot->is_alive, false);
        atomic_fetch_sub(&ctx->worker_pool->active_count, 1);
    }
    atomic_store(&slot->state, WORKER_STATE_DEAD);  /* v15.1.0 */
    slot->pid = -1;
}

/* ================================================================
 * v15.5.8: dspill 派发兜底（运行级追加文件，替代 pbin 滑动窗口）
 * ================================================================ */

/**
 * @brief  将 HIGH_WATER 跳推的目录追加到 dspill 兜底文件
 * @param  ctx   AppContext*        应用上下文
 * @param  path  const char*        目录路径，不能为空
 * @param  st    const struct stat* 目录 stat，允许为 NULL
 * @return void
 *
 * @note   懒打开 {base}.dspill（"ab"），复用 pbin 记录格式；每条追加后 fflush
 *         （write 页缓存，非 fsync），保证加载器立即可见。
 *         兜底文件不可用时记入熔断清单并强行入队——宁可队列膨胀也不丢目录。
 *         由 batch_processor（线程池线程）调用，与主线程的加载器经 dspill_mutex 互斥。
 */
void dspill_append(AppContext *ctx, const char *path, const struct stat *st) {
    if (!ctx || !path || !ctx->cfg.progress_base) return;

    pthread_mutex_lock(&ctx->dspill_mutex);
    if (!ctx->dspill_fp) {
        char *spill_path = get_dspill_filename(ctx->cfg.progress_base);
        ctx->dspill_fp = fopen(spill_path, "ab");
        free(spill_path);
        if (!ctx->dspill_fp) {
            circuit_breaker_record(ctx, "DSPILL_IO", path, st ? st->st_dev : 0, 0);
            char *dup = strdup(path);
            if (!dispatch_queue_push(&ctx->dispatch_queue, dup, st)) {
                free(dup);
            }
            pthread_mutex_unlock(&ctx->dspill_mutex);
            return;
        }
        setvbuf(ctx->dspill_fp, NULL, _IOFBF, 64 * 1024);
    }

    write_pbin_record(ctx->dspill_fp, path, st);
    fflush(ctx->dspill_fp);
    ctx->dspill_appended++;
    pthread_mutex_unlock(&ctx->dspill_mutex);
}

/**
 * @brief  从 dspill 兜底文件回填目录到 dispatch_queue
 * @param  ctx     AppContext*  应用上下文
 * @param  target  int          本次最多回填的目录数量
 * @return int     实际回填的目录数量；0 表示已消费到 EOF
 *
 * @note   按 dspill_read_offset 字节游标顺序读取；游标只在记录成功入队后前进，
 *         入队失败（队列满）时回退游标，记录不得丢失（v15.5.1 游标越记丢失教训）。
 *         dspill 为运行级追加文件，无分片轮转、无删除竞争。
 */
int load_dirs_from_dspill(AppContext *ctx, int target) {
    if (!ctx || target <= 0) return 0;

    pthread_mutex_lock(&ctx->dspill_mutex);
    if (!ctx->dspill_fp) {
        pthread_mutex_unlock(&ctx->dspill_mutex);
        return 0;
    }

    fflush(ctx->dspill_fp); /* 确保写缓冲对读端可见 */

    char *spill_path = get_dspill_filename(ctx->cfg.progress_base);
    FILE *fp = fopen(spill_path, "rb");
    free(spill_path);
    if (!fp) {
        pthread_mutex_unlock(&ctx->dspill_mutex);
        return 0;
    }

    if (fseek(fp, ctx->dspill_read_offset, SEEK_SET) != 0) {
        fclose(fp);
        pthread_mutex_unlock(&ctx->dspill_mutex);
        return 0;
    }

    int loaded = 0;
    while (loaded < target) {
        long rec_start = ftell(fp);
        char *path = NULL;
        struct stat st;
        unsigned char d_type;

        if (!read_next_pbin_record(fp, &path, &st, &d_type)) {
            break; /* EOF 或尾部不完整记录（追加中） */
        }
        ctx->dspill_read_offset = ftell(fp);

        if (d_type == DT_DIR) {
            if (!dispatch_queue_push(&ctx->dispatch_queue, path, &st)) {
                free(path);
                ctx->dspill_read_offset = rec_start; /* 回退游标，下轮重试 */
                break;
            }
            loaded++;
        } else {
            free(path); /* dspill 只应含目录；防御性跳过 */
        }
    }

    fclose(fp);
    if (loaded > 0) {
        ctx->dspill_loaded += (unsigned long)loaded;
        log_info("[DspillLoader] loaded %d dirs from dspill (offset=%ld)",
                 loaded, ctx->dspill_read_offset);
    }
    pthread_mutex_unlock(&ctx->dspill_mutex);
    return loaded;
}

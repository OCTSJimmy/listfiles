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
    /* v15.6.0: 每次派发分配递增 epoch；发送失败则回滚计数器，保持账目一致 */
    uint64_t epoch = atomic_fetch_add(&ctx->epoch_counter, 1) + 1;
    scan->epoch = epoch;
    safe_strcpy(scan->path, path, sizeof(scan->path));

    IpcThreadMsg msg = {
        .type = CMD_SCAN,
        .slot_id = wid,
        .data = scan,
        .data_len = sizeof(*scan),
        .epoch = 0
    };

    if (!msg_queue_send(ctx->ipc_cmd_queues[wid], &msg)) {
        atomic_fetch_sub(&ctx->epoch_counter, 1);  /* 回滚未生效的 epoch */
        log_warn_v(202607030000UL, "[Dispatch] cmd_queue[%d] full, dropping %s", wid, path_log_mask(path));
        free(scan);
        return false;
    }
    /* 发送成功：epoch 生效，写入 slot 供 RET_BATCH/RET_FINISH 校验 */
    ctx->worker_pool->slots[wid].current_epoch = epoch;
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
        .data_len = sizeof(*rep),
        .epoch = 0
    };

    /* v15.6.0: REPLACE 丢失会导致 IPC 线程永久等待新 fd，队列满时短暂重试，
     * 仍失败属设计外异常（容量 65536），log_fatal 暴露，不得静默丢弃 */
    int retry = 0;
    while (!msg_queue_send(ctx->ipc_cmd_queues[wid], &msg)) {
        if (++retry > 100) {
            log_fatal("[Replace] cmd_queue[%d] full after 100 retries, REPLACE undeliverable", wid);
            free(rep);
            return;
        }
        usleep(1000);
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
        /* 计数器必须始终取模回卷：v15.6.0 自适应零等待主循环使本函数在
         * "全部 Worker 忙 + 队列非空" 时以内存速度空转，plain int 约 2 分钟
         * 即可溢出为负值，candidate 变负 → slots[-15] 野读段错误（回归实测）。 */
        int candidate = ctx->next_dispatch_worker % num_workers;
        ctx->next_dispatch_worker = (candidate + 1) % num_workers;
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
        /* v15.5.0: pending_tasks++ only on successful dispatch */
        atomic_fetch_add(&ctx->pending_tasks, 1);
        /* v15.6.0: 在途任务屏障初始化（P0-001）。dpbin_append 从此处迁走——
         * 改由 main_loop.c advance_task_barriers 在该任务全部 BATCH 处理完且
         * 输出 COMMITTED 后写入；此处仅保存目录 stat 供届时使用。 */
        slot->current_st = task.st;
        atomic_store(&slot->batches_received, 0);
        atomic_store(&slot->batches_processed, 0);
        slot->task_state = DT_SCANNING;
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
    if (wid < 0 || wid >= MAX_WORKERS) return false;

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
        /* v15.6.0（P0-005）：目录级熔断达阈值统一写 spbin（CIRCUIT_BREAKER），
         * 恢复时永久跳过，不再经泵送盲目重入队 */
        dev_t dev = 0;
        if (ctx->worker_pool && wid < ctx->worker_pool->num_workers) {
            dev = (dev_t)ctx->worker_pool->slots[wid].current_dev;
        }
        spbin_write_record(ctx, path, SP_REASON_CIRCUIT_BREAKER, dev);
        return true; /* 熔断：不再重试 */
    }
    return false; /* 未熔断：允许重试 */
}

/* ================================================================
 * v15.6.0（P1-004）：毒丸目录致死计数
 * ================================================================ */

#define POISON_DEATH_THRESHOLD 3  /* 同一目录累计致死 Worker 此次数后永久隔离 */

/**
 * @brief  记录一次"Worker 死亡时正在扫描该目录"的致死事件
 * @param  ctx   AppContext*  应用上下文，不能为空
 * @param  path  const char*  致死时 Worker 正在扫描的目录路径，不能为空
 * @return int   该路径的累计致死次数（含本次）
 *
 * @note   毒丸计数的是"Worker 死亡（DEV_TIMEOUT/heartbeat/崩溃）时正在扫描该目录"，
 *         与 RET_ERROR 的设备级错误不同，不计入设备级错误统计（不触碰 dev_mgr）。
 *         Worker 死亡是稀有事件，小型动态数组 + 线性查找即可。
 */
static int poison_note_death(AppContext *ctx, const char *path) {
    for (size_t i = 0; i < ctx->poison_count; i++) {
        if (strcmp(ctx->poison_paths[i], path) == 0) {
            return ++ctx->poison_counts[i];
        }
    }
    if (ctx->poison_count >= ctx->poison_capacity) {
        size_t new_cap = ctx->poison_capacity ? ctx->poison_capacity * 2 : 16;
        /* 逐个 realloc 并立即写回：失败时原指针仍有效，不产生悬垂 */
        char **new_paths = realloc(ctx->poison_paths, new_cap * sizeof(char *));
        if (!new_paths) return 1; /* OOM：不计数也不隔离，保守继续 */
        ctx->poison_paths = new_paths;
        int *new_counts = realloc(ctx->poison_counts, new_cap * sizeof(int));
        if (!new_counts) return 1;
        ctx->poison_counts = new_counts;
        ctx->poison_capacity = new_cap;
    }
    ctx->poison_paths[ctx->poison_count] = strdup(path);
    if (!ctx->poison_paths[ctx->poison_count]) return 1;
    ctx->poison_counts[ctx->poison_count] = 1;
    ctx->poison_count++;
    return 1;
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

    /* v15.6.0: 在途任务屏障复位（P0-001）——重派发时由 dispatch_from_queue 重新初始化 */
    atomic_store(&slot->batches_received, 0);
    atomic_store(&slot->batches_processed, 0);
    slot->task_state = DT_NONE;

    /* v15.5.3: Circuit breaker for DEV_TIMEOUT redispatch loop
     * v15.5.9: 增加指数退避——同一目录连续超时后，redispatch 前等待
     * 30s -> 120s -> 300s，给 NFS 大目录喘息时间
     * v15.6.0（P1-004）：毒丸隔离优先——同一目录累计致死 Worker 3 次
     * 直接写 spbin POISON 永久隔离，不再重入队，不计入设备级错误统计 */
    if (redispatch_current && slot->current_path[0] != '\0') {
        int deaths = poison_note_death(ctx, slot->current_path);
        if (deaths >= POISON_DEATH_THRESHOLD) {
            log_warn("[Poison] 目录已累计致死 Worker %d 次，隔离进 spbin(POISON) 永久跳过: %s",
                     deaths, path_log_mask(slot->current_path));
            spbin_write_record(ctx, slot->current_path, SP_REASON_POISON, (dev_t)slot->current_dev);
            circuit_breaker_record(ctx, "POISON", slot->current_path,
                                   (dev_t)slot->current_dev, deaths);
            /* 毒丸目录不重入队，pending_tasks 已在上方递减 */
        } else {
            bool tripped = circuit_breaker_check(ctx, worker_id, slot->current_path);
            if (!tripped) {
                /* 计算退避时间：基于已超时次数 */
                int backoff_sec = 0;
                if (ctx->timeout_counts[worker_id] == 1) backoff_sec = 30;
                else if (ctx->timeout_counts[worker_id] == 2) backoff_sec = 120;
                else if (ctx->timeout_counts[worker_id] >= 3) backoff_sec = 300;

                if (backoff_sec > 0) {
                    ctx->redispatch_backoff_until[worker_id] = time(NULL) + backoff_sec;
                    log_info_v(202608202330UL, "[CircuitBreaker] Path timeout count=%d, backoff %ds before redispatch: %s",
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
    }

    if (atomic_load(&slot->is_alive)) {
        atomic_store(&slot->is_alive, false);
        atomic_fetch_sub(&ctx->worker_pool->active_count, 1);
    }
    atomic_store(&slot->state, WORKER_STATE_DEAD);  /* v15.1.0 */
    slot->pid = -1;
}

/* ================================================================
 * v15.6.0: 统一入队入口 enqueue_dir（P0-004 统一队列模型）
 * ================================================================ */

/**
 * @brief  统一目录入队入口——所有待扫描目录的唯一入口
 * @param  ctx   AppContext*        应用上下文，不能为空
 * @param  path  const char*        目录路径，不能为空
 * @param  st    const struct stat* 目录 stat，允许为 NULL（spbin 重入队等无 stat 场景）
 * @return void
 *
 * @note   调用点：batch_processor 新发现目录、pump_pbin_batch 恢复泵送、
 *         spbin_requeue_recovered 设备恢复重入队、restore_progress 的 dspill 回填。
 *         语义顺序：
 *         1. 查 completed_set：已完成目录不再入队（差集剪枝）；
 *         2. 查 enqueued_set：已入队（在 dispatch_queue 或 dspill 中）直接返回；
 *            不在则插入 enqueued_set（防重复入队）；
 *         3. dispatch_queue < HIGH_WATER 或无 progress_base → dispatch_queue_push；
 *            否则 dspill_append 兜底。push 失败时 enqueued_set 语义上无删除，
 *            转 dspill_append 兜底即可（不丢目录）。
 *         注意：dspill 运行时回填（load_dirs_from_dspill）是"搬运"而非新入队
 *         ——条目写 dspill 时已标记 enqueued_set，不回查、不经本函数。
 */
void enqueue_dir(AppContext *ctx, const char *path, const struct stat *st) {
    if (!ctx || !path) return;

    uint8_t fp[FP_SIZE];
    fp_compute(path, st ? st->st_dev : 0, st ? st->st_ino : 0, fp);

    /* 1. 已完成剪枝（differential resume）——仅当该目录同时在 discovered_set
     *    （pbin 有其发现记录，子树可由泵送/重扫闭环）才剪枝。
     *    非续传运行崩溃后 pbin 无记录：根目录等不在 discovered_set，
     *    若仅按 completed_set 剪枝会导致整棵子树永久漏扫（v15.6.0 回归实测）。 */
    if (ctx->completed_set && fp_set_contains(ctx->completed_set, fp)
        && ctx->discovered_set && fp_set_contains(ctx->discovered_set, fp)) {
        return;
    }

    /* 2. 防重复入队 */
    if (ctx->enqueued_set && fp_set_insert(ctx->enqueued_set, fp)) {
        return;
    }

    /* 3. 入队：内存队列优先；超 HIGH_WATER 或 push 失败转 dspill 兜底。
     *    无 progress_base 时无兜底通道，宁可队列膨胀也不丢目录。 */
    if (dispatch_queue_count(&ctx->dispatch_queue) < DISPATCH_QUEUE_HIGH_WATER
        || !ctx->cfg.progress_base) {
        char *dup = strdup(path);
        if (dup && dispatch_queue_push(&ctx->dispatch_queue, dup, st)) {
            return;
        }
        free(dup);
    }
    dspill_append(ctx, path, st);
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
 * @note   懒打开 {base}.dspill（"ab"），复用 pbin 记录格式。
 *         v15.6.0（P0-004）：不再每条 fflush——内存缓冲刷盘策略为
 *         "1000 条或 1 秒，先到先刷"（条数在此统计，定时刷盘由主循环
 *         dspill_flush_check 触发；加载器读取前也会先 fflush 保证可见）。
 *         崩溃丢失可接受：dspill 条目在发现时已写 pbin（或父目录未完成会被
 *         重扫重新发现），pbin 泵送与 Reset 援救兜底，不丢目录（at-least-once）。
 *         兜底文件不可用时记入熔断清单并强行入队——宁可队列膨胀也不丢目录。
 *         须持 dspill_mutex 与主线程的加载器互斥。
 */
#define DSPILL_FLUSH_BATCH 1000  /* v15.6.0: dspill 内存缓冲刷盘批量（另挂 1 秒定时刷盘） */
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
        ctx->dspill_last_flush = time(NULL);
    }

    write_pbin_record(ctx->dspill_fp, path, st);
    ctx->dspill_appended++;
    ctx->dspill_pending++;
    if (ctx->dspill_pending >= DSPILL_FLUSH_BATCH) {
        fflush(ctx->dspill_fp);
        ctx->dspill_pending = 0;
        ctx->dspill_last_flush = time(NULL);
    }
    pthread_mutex_unlock(&ctx->dspill_mutex);
}

/**
 * @brief  dspill 定时刷盘检查（v15.6.0，P0-004）
 * @param  ctx    AppContext*  应用上下文
 * @param  force  bool         true 表示无视计数与时间强制刷盘（完结检查前调用）
 * @return void
 *
 * @note   挂点：主循环每轮 dspill 回填检查后调用（force=false，距上次刷盘
 *         超过 1 秒且有待刷条目时刷盘）；完结硬性断言前调用（force=true，
 *         保证残留字节统计基于落盘后的文件大小）。
 */
void dspill_flush_check(AppContext *ctx, bool force) {
    if (!ctx) return;
    pthread_mutex_lock(&ctx->dspill_mutex);
    if (ctx->dspill_fp && ctx->dspill_pending > 0
        && (force || time(NULL) - ctx->dspill_last_flush >= 1)) {
        fflush(ctx->dspill_fp);
        ctx->dspill_pending = 0;
        ctx->dspill_last_flush = time(NULL);
    }
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
    ctx->dspill_pending = 0;
    ctx->dspill_last_flush = time(NULL);

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

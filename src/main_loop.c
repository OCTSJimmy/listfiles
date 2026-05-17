#define _GNU_SOURCE
#include "main_loop.h"
#include "utils.h"
#include "progress.h"
#include "worker_proc.h"
#include "log.h"
#include "msg_format.h"
#include "msg_queue.h"
#include "ipc_thread.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include <sys/wait.h>
#include <signal.h>
#include <stdatomic.h>
#include <dirent.h>
#include <sys/eventfd.h>

/* ================================================================
 * ParsedBatch helpers (unchanged from v12.x)
 * ================================================================ */

typedef struct {
    char **paths;
    struct stat *stats;
    int count;
} ParsedBatch;

static void parsed_batch_free(ParsedBatch *b) {
    if (!b) return;
    for (int i = 0; i < b->count; i++) free(b->paths[i]);
    free(b->paths);
    free(b->stats);
    b->paths = NULL;
    b->stats = NULL;
    b->count = 0;
}

static bool parse_batch(const uint8_t *payload, uint32_t len, ParsedBatch *out) {
    memset(out, 0, sizeof(*out));
    if (len < sizeof(IpcBatchHeader)) return false;

    const uint8_t *p = payload;
    IpcBatchHeader bh;
    memcpy(&bh, p, sizeof(bh));
    p += sizeof(bh);

    out->paths = calloc(bh.count, sizeof(char*));
    out->stats = calloc(bh.count, sizeof(struct stat));
    if (!out->paths || !out->stats) goto fail;

    for (uint32_t i = 0; i < bh.count; i++) {
        if ((size_t)(p - payload) + sizeof(uint32_t) > len) goto fail;
        uint32_t plen;
        memcpy(&plen, p, sizeof(plen));
        p += sizeof(plen);

        if ((size_t)(p - payload) + plen + sizeof(struct stat) > len) goto fail;

        out->paths[i] = malloc(plen + 1);
        if (!out->paths[i]) goto fail;
        memcpy(out->paths[i], p, plen);
        out->paths[i][plen] = '\0';
        p += plen;

        memcpy(&out->stats[i], p, sizeof(struct stat));
        p += sizeof(struct stat);
        out->count++;
    }
    return true;

fail:
    parsed_batch_free(out);
    return false;
}

/* ================================================================
 * Thread pool callback: CPU-intensive deduplication (unchanged)
 * ================================================================ */

static void batch_dedup_worker(TPBatch *batch, void *user_data) {
    /* v15.1.4: defensive sanity check */
    if (!batch || batch->count < 0 || batch->count > 1000000) {
        log_fatal("[DedupWorker] batch invalid or count out of range: %p count=%d",
                  (void*)batch, batch ? batch->count : -999);
        return;
    }

    AppContext *ctx = user_data;
    const int ITERATION_LIMIT = 100000; /* v15.1.2: hard timeout for single batch */
    for (int i = 0; i < batch->count; i++) {
        if (i >= ITERATION_LIMIT) {
            log_fatal("[DedupWorker] batch iteration exceeded limit %d (count=%d). Aborting to prevent CPU spin.",
                      ITERATION_LIMIT, batch->count);
            /* abort() removed for production safety; mark all remaining as duplicate */
            for (int j = i; j < batch->count; j++) {
                batch->results[j] = 1; /* duplicate (skip) */
            }
            return;
        }
        const char *path = batch->paths[i];
        struct stat *st = &batch->stats[i];
        uint8_t fp[FP_SIZE];
        fp_compute(path, st->st_dev, st->st_ino, fp);
        uint8_t result = 0;
        if (fp_set_insert(ctx->visited_set, fp)) {
            result |= 1; /* duplicate */
        }
        if (dev_mgr_is_blacklisted(ctx->dev_mgr, st->st_dev)) {
            result |= 2; /* blacklisted */
        }
        batch->results[i] = result;
    }
}

/* ================================================================
 * IPC helper: send CMD_SCAN to IPC thread
 * ================================================================ */

static bool send_scan_to_ipc(AppContext *ctx, int wid, const char *path, uint64_t dev) {
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
        log_warn("[Dispatch] cmd_queue[%d] full, dropping %s", wid, path_log_mask(path));
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

static void send_stop_to_ipc(AppContext *ctx, int wid) {
    IpcThreadMsg msg = {
        .type = CMD_STOP,
        .slot_id = wid,
        .data = NULL,
        .data_len = 0
    };
    msg_queue_send(ctx->ipc_cmd_queues[wid], &msg);
}

/* ================================================================
 * Side effects for a completed batch (must run on main thread)
 * ================================================================ */

static void process_completed_batch(AppContext *ctx, TPBatch *batch) {
    /* v15.1.4: defensive sanity check to prevent CPU spin from corrupted count */
    if (!batch || batch->count < 0 || batch->count > 1000000) {
        log_fatal("[Batch] batch invalid or count out of range: %p count=%d, worker=%d. Dropping.",
                  (void*)batch, batch ? batch->count : -999,
                  batch ? batch->worker_id : -999);
        if (batch) {
            for (int i = 0; i < batch->count && i < 1000000; i++) free(batch->paths[i]);
            free(batch->paths);
            free(batch->stats);
            free(batch->results);
            free(batch);
        }
        atomic_fetch_sub(&ctx->pending_batches, 1);
        return;
    }

    log_debug("[Batch] process_completed_batch start worker=%d count=%d pending_batches=%ld",
              batch->worker_id, batch->count, atomic_load(&ctx->pending_batches));
    OutputBatch out_batch = {0};

    const int PROC_ITER_LIMIT = 1000000; /* v15.1.4 */
    for (int i = 0; i < batch->count; i++) {
        if (i >= PROC_ITER_LIMIT) {
            log_fatal("[Batch] process_completed_batch iteration limit exceeded: worker=%d count=%d i=%d. Aborting batch.",
                      batch->worker_id, batch->count, i);
            break;
        }
        const char *path = batch->paths[i];
        struct stat *st = &batch->stats[i];
        uint8_t result = batch->results[i];

        if (result & 1) continue; /* duplicate */
        if (result & 2) {
            ctx->state.has_error = true;
            continue; /* blacklisted */
        }

        if (S_ISDIR(st->st_mode)) {
            if (ctx->hist_pump_state == HIST_PUMP_OLD) {
                fpbin_append(ctx, path, st);
            } else {
                atomic_fetch_add(&ctx->pending_tasks, 1);
                /* v15.1.0: 只选择 STATE_IDLE 的 Worker */
                int wid = -1;
                int attempts = 0;
                int num_workers = ctx->worker_pool->num_workers;
                while (attempts < num_workers) {
                    int candidate = ctx->next_dispatch_worker % num_workers;
                    ctx->next_dispatch_worker++;
                    WorkerSlot *cand_slot = &ctx->worker_pool->slots[candidate];
                    if (!atomic_load(&cand_slot->is_alive)) { attempts++; continue; }
                    if (atomic_load(&cand_slot->state) != WORKER_STATE_IDLE) { attempts++; continue; }
                    wid = candidate;
                    break;
                }
                if (wid < 0) {
                    log_warn("[Dispatch] no IDLE worker available (path=%s), requeue to lost_tasks", path_log_mask(path));
                    atomic_fetch_sub(&ctx->pending_tasks, 1);
                    lost_tasks_push(&ctx->lost_tasks, strdup(path));
                    continue;
                }
                WorkerSlot *slot = &ctx->worker_pool->slots[wid];
                atomic_store(&slot->state, WORKER_STATE_BUSY);
                slot->current_dev = st->st_dev;
                safe_strcpy(slot->current_path, path, sizeof(slot->current_path));
                if (!send_scan_to_ipc(ctx, wid, path, st->st_dev)) {
                    atomic_fetch_sub(&ctx->pending_tasks, 1);
                    atomic_store(&slot->state, WORKER_STATE_IDLE);
                }
            }

            ctx->state.dir_count++;
            if (ctx->cfg.include_dir) {
                OutputTask *task = calloc(1, sizeof(OutputTask));
                task->path = strdup(path);
                task->st = *st;
                if (out_batch.tail) {
                    out_batch.tail->next = task;
                } else {
                    out_batch.head = task;
                }
                out_batch.tail = task;
                out_batch.count++;
            }
            if (ctx->cfg.print_dir && ctx->state.dir_info_fp && !ctx->cfg.mute) {
                fprintf(ctx->state.dir_info_fp, "%s%s\n", OUTPUT_DIR_PREFIX, path);
            }
            if (ctx->cfg.continue_mode && ctx->hist_pump_state != HIST_PUMP_OLD) {
                record_path_batch_append(&ctx->cfg, &ctx->state, &ctx->record_batch, path, st);
            }
        } else {
            ctx->state.file_count++;
            OutputTask *task = calloc(1, sizeof(OutputTask));
            task->path = strdup(path);
            task->st = *st;
            if (out_batch.tail) {
                out_batch.tail->next = task;
            } else {
                out_batch.head = task;
            }
            out_batch.tail = task;
            out_batch.count++;
            if (ctx->cfg.continue_mode) {
                record_path_batch_append(&ctx->cfg, &ctx->state, &ctx->record_batch, path, st);
            }
        }

        if (out_batch.count >= ASYNC_BATCH_SIZE) {
            async_writer_submit_batch(ctx->async_writer, &out_batch);
            out_batch.head = NULL;
            out_batch.tail = NULL;
            out_batch.count = 0;
        }
    }

    if (out_batch.count > 0) {
        async_writer_submit_batch(ctx->async_writer, &out_batch);
    }

    atomic_fetch_sub(&ctx->pending_batches, 1);
    log_debug("[Batch] pending_batches after sub: %ld", atomic_load(&ctx->pending_batches));
    ctx->state.total_dequeued_count++;

    for (int i = 0; i < batch->count; i++) free(batch->paths[i]);
    free(batch->paths);
    free(batch->stats);
    free(batch->results);
    free(batch);
}

/* ================================================================
 * Dispatch lost tasks (v13.0.0: send via cmd_queue)
 * ================================================================ */

static void dispatch_lost_tasks(AppContext *ctx) {
    /* 短路：如果所有 Worker 都死了，直接返回 */
    bool any_alive = false;
    for (int i = 0; i < ctx->worker_pool->num_workers; i++) {
        if (atomic_load(&ctx->worker_pool->slots[i].is_alive)) {
            any_alive = true;
            break;
        }
    }
    if (!any_alive) return;

    char *path;
    while (lost_tasks_pop(&ctx->lost_tasks, &path)) {
        if (!path) continue;

        /* v15.1.0: 只选择 STATE_IDLE 的 Worker */
        int wid = -1;
        int attempts = 0;
        int num_workers = ctx->worker_pool->num_workers;
        while (attempts < num_workers) {
            int candidate = ctx->next_dispatch_worker % num_workers;
            ctx->next_dispatch_worker++;
            WorkerSlot *cand_slot = &ctx->worker_pool->slots[candidate];
            if (!atomic_load(&cand_slot->is_alive)) { attempts++; continue; }
            if (atomic_load(&cand_slot->state) != WORKER_STATE_IDLE) { attempts++; continue; }
            wid = candidate;
            break;
        }
        if (wid < 0) {
            log_warn("[LostTasks] no IDLE worker available, requeue %s", path_log_mask(path));
            lost_tasks_push(&ctx->lost_tasks, path);
            break; /* 停止继续尝试，等下一轮 */
        }
        WorkerSlot *slot = &ctx->worker_pool->slots[wid];
        atomic_store(&slot->state, WORKER_STATE_BUSY);

        if (!send_scan_to_ipc(ctx, wid, path, 0)) {
            atomic_store(&slot->state, WORKER_STATE_IDLE);
            lost_tasks_push(&ctx->lost_tasks, path);
            continue;
        }
        atomic_fetch_add(&ctx->pending_tasks, 1);
        log_debug("[LostTasks] dispatched %s to worker %d, pending_tasks=%ld", path_log_mask(path), wid, atomic_load(&ctx->pending_tasks));

        slot->current_dev = 0;
        safe_strcpy(slot->current_path, path, sizeof(slot->current_path));
        free(path);
    }
    lost_tasks_compact(&ctx->lost_tasks);
}

static void drain_completed_batches(AppContext *ctx) {
    TPBatch *batch;
    while ((batch = thread_pool_poll_completed(ctx->thread_pool)) != NULL) {
        process_completed_batch(ctx, batch);
    }
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
            log_debug("[Cleanup] Worker %d drained %d orphaned tasks from fd_cmd_rd", worker_id, orphaned);
        }
        close(slot->fd_cmd_rd);
        slot->fd_cmd_rd = -1;
    }

    /* Migrate backlog to lost_tasks */
    lost_tasks_push_backlog(&ctx->lost_tasks, slot->backlog_paths, slot->backlog_count);
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

    if (redispatch_current && slot->current_path[0] != '\0') {
        if (lost_tasks_push(&ctx->lost_tasks, strdup(slot->current_path))) {
            atomic_fetch_add(&ctx->pending_tasks, 1);
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
 * Handle return messages from IPC threads
 * ================================================================ */

static void handle_return_message(AppContext *ctx, IpcThreadMsg *msg) {
    log_debug("[Bus] received type=%u slot=%d len=%zu", msg->type, msg->slot_id, msg->data_len);
    switch (msg->type) {
        case RET_BATCH: {
            log_debug("[Bus] Worker %d BATCH (len=%zu)", msg->slot_id, msg->data_len);
            main_loop_handle_batch(ctx, msg->slot_id, msg->data, msg->data_len);
            break;
        }
        case RET_HEARTBEAT: {
            if (msg->data_len >= sizeof(RetHeartbeatPayload)) {
                RetHeartbeatPayload *hb = (RetHeartbeatPayload*)msg->data;
                main_loop_handle_heartbeat(ctx, msg->slot_id, hb->timestamp);
            }
            break;
        }
        case RET_ERROR: {
            if (msg->data_len >= sizeof(RetErrorPayload)) {
                RetErrorPayload *err = (RetErrorPayload*)msg->data;
                IpcErrorHeader hdr = { err->errno_code, err->dev };
                main_loop_handle_error(ctx, msg->slot_id, &hdr, err->path);
                /* v15.1.1: 设备级错误不替换 Worker，Worker 回到 IDLE */
                atomic_store(&ctx->worker_pool->slots[msg->slot_id].state, WORKER_STATE_IDLE);
            }
            break;
        }
        case RET_READY: {
            log_info("[Bus] Worker %d READY", msg->slot_id);
            atomic_store(&ctx->worker_pool->slots[msg->slot_id].last_heartbeat, time(NULL));
            /* v15.1.1: 无论之前是 INITIALIZING 还是其他状态，收到 READY 后置 IDLE */
            atomic_store(&ctx->worker_pool->slots[msg->slot_id].state, WORKER_STATE_IDLE);
            break;
        }
        case RET_FINISH: {
            log_info("[Bus] Worker %d FINISH (pending_tasks=%ld)", msg->slot_id, atomic_load(&ctx->pending_tasks));
            atomic_fetch_sub(&ctx->pending_tasks, 1);
            atomic_store(&ctx->worker_pool->slots[msg->slot_id].state, WORKER_STATE_IDLE); /* v15.1.0 */
            /* Task completed, Worker is now IDLE */
            break;
        }
        case RET_DEAD: {
            WorkerSlot *slot = &ctx->worker_pool->slots[msg->slot_id];
            if (atomic_load(&slot->is_alive) && slot->pid != -1) {
                /* Stale RET_DEAD after replacement; ignore */
                break;
            }
            log_error("[Bus] Worker %d DEAD reported by IPC thread", msg->slot_id);
            cleanup_dead_worker_slot(ctx, msg->slot_id, true);
            break;
        }
        case RET_DEV_TIMEOUT: {
            log_error("[Bus] Worker %d DEV_TIMEOUT (scanner stuck), replacing", msg->slot_id);
            cleanup_dead_worker_slot(ctx, msg->slot_id, true);
            break;
        }
        case RET_EXIT: {
            log_info("[Bus] Worker %d normal exit", msg->slot_id);
            cleanup_dead_worker_slot(ctx, msg->slot_id, false);
            break;
        }
        case MSG_DROP: {
            if (msg->data_len >= sizeof(DropPayload)) {
                DropPayload *drop = (DropPayload*)msg->data;
                if (!lost_tasks_push(&ctx->lost_tasks, strdup(drop->path))) {
                    log_warn("[Bus] MSG_DROP requeue failed: %s", path_log_mask(drop->path));
                }
            }
            break;
        }
        default: {
            log_error("[Bus] Worker %d UNKNOWN message type=%u (len=%zu)",
                     msg->slot_id, msg->type, msg->data_len);
            break;
        }
    }
    free(msg->data);
    msg->data = NULL;
}

/* ================================================================
 * Message handlers (mostly unchanged, accept payload directly)
 * ================================================================ */

void main_loop_handle_batch(AppContext *ctx, int worker_id, const void *payload, uint32_t len) {
    ParsedBatch parsed;
    if (!parse_batch(payload, len, &parsed)) {
        log_error("[Batch] Worker %d parse_batch FAILED (len=%u)", worker_id, len);
        return;
    }
    log_debug("[Batch] Worker %d parse_batch OK (count=%d)", worker_id, parsed.count);

    /* v15.1.4: defensive sanity check on parsed.count */
    if (parsed.count < 0 || parsed.count > 1000000) {
        log_fatal("[Batch] parsed.count out of range: %d (worker=%d, len=%u), dropping batch",
                  parsed.count, worker_id, len);
        parsed_batch_free(&parsed);
        return;
    }

    uint8_t *results = calloc((size_t)parsed.count, 1);
    if (!results) {
        parsed_batch_free(&parsed);
        return;
    }

    TPBatch *batch = malloc(sizeof(TPBatch));
    if (!batch) {
        free(results);
        parsed_batch_free(&parsed);
        return;
    }
    batch->paths = parsed.paths;
    batch->stats = parsed.stats;
    batch->count = parsed.count;
    batch->results = results;
    batch->worker_id = worker_id;

    log_debug("[Batch] pending_batches before add: %ld", atomic_load(&ctx->pending_batches));
    atomic_fetch_add(&ctx->pending_batches, 1);
    log_debug("[Batch] pending_batches after add: %ld", atomic_load(&ctx->pending_batches));
    if (thread_pool_submit(ctx->thread_pool, batch)) {
        log_debug("[Batch] Worker %d submitted to thread pool", worker_id);
        return;
    }

    log_debug("[Batch] Worker %d thread pool full, inline processing", worker_id);
    batch_dedup_worker(batch, ctx);
    process_completed_batch(ctx, batch);
}

void main_loop_handle_heartbeat(AppContext *ctx, int worker_id, uint64_t timestamp) {
    if (worker_id < 0 || worker_id >= ctx->worker_pool->num_workers) return;
    atomic_store(&ctx->worker_pool->slots[worker_id].last_heartbeat, (time_t)timestamp);
}

void main_loop_handle_error(AppContext *ctx, int worker_id, const IpcErrorHeader *err, const char *path) {
    (void)worker_id;
    if (err->errno_code == ETIMEDOUT || err->errno_code == EIO) {
        dev_t dev = (dev_t)err->dev;
        log_error("[Monitor] Worker error on dev %lu: %s (errno=%d)",
                (unsigned long)dev, path, err->errno_code);

        if (dev_mgr_get_state(ctx->dev_mgr, dev) != DEV_STATE_DEAD) {
            dev_mgr_mark_dead(ctx->dev_mgr, dev);
            ctx->state.has_error = true;

            SpbinEntry entry = {0};
            entry.path = strdup(path);
            entry.dev = dev;
            entry.blacklist_time = time(NULL);
            entry.retry_count = 0;
            entry.probe_interval = PROBE_INTERVAL_INITIAL;
            entry.d_type = DT_DIR;
            entry.s_status = SP_STATUS_PROBING;
            spbin_append(ctx, &entry);

            ProbeTask task = {0};
            task.dev = dev;
            safe_strcpy(task.probe_path, path, sizeof(task.probe_path));
            task.next_probe_time = time(NULL) + PROBE_INTERVAL_INITIAL;
            task.probe_interval = PROBE_INTERVAL_INITIAL;
            task.retry_count = 0;
            task.s_status = SP_STATUS_PROBING;
            probe_scheduler_push(ctx->probe_scheduler, &task);
        }
    }
}

void main_loop_handle_exit(AppContext *ctx, int worker_id) {
    if (worker_id < 0 || worker_id >= ctx->worker_pool->num_workers) return;
    WorkerSlot *slot = &ctx->worker_pool->slots[worker_id];
    log_info("[Exit] Worker %d normal exit (pid=%d). active=%d->%d",
            worker_id, slot->pid,
            atomic_load(&ctx->worker_pool->active_count),
            atomic_load(&ctx->worker_pool->active_count) - 1);
    int status;
    waitpid(slot->pid, &status, WNOHANG);
    cleanup_dead_worker_slot(ctx, worker_id, false);
}

/* ================================================================
 * IPC Thread lifecycle helpers
 * ================================================================ */

bool init_ipc_threads(AppContext *ctx) {
    int n = ctx->worker_pool->num_workers;

    ctx->ipc_cmd_queues = calloc(n, sizeof(MsgQueue*));
    ctx->ipc_ret_queues = calloc(n, sizeof(MsgQueue*));
    ctx->ipc_threads = calloc(n, sizeof(IpcThreadCtx*));
    ctx->ipc_tids = calloc(n, sizeof(pthread_t));
    if (!ctx->ipc_cmd_queues || !ctx->ipc_ret_queues || !ctx->ipc_threads || !ctx->ipc_tids) {
        log_fatal("IPC thread arrays allocation failed");
        return false;
    }

    pthread_mutex_init(&ctx->main_mutex, NULL);
    pthread_cond_init(&ctx->main_cond, NULL);
    atomic_init(&ctx->main_wakeup, false);

    for (int i = 0; i < n; i++) {
        ctx->ipc_cmd_queues[i] = msg_queue_create(MSG_QUEUE_DEFAULT_CAPACITY);
        ctx->ipc_ret_queues[i] = msg_queue_create(MSG_QUEUE_DEFAULT_CAPACITY);
        if (!ctx->ipc_cmd_queues[i] || !ctx->ipc_ret_queues[i]) {
            log_fatal("msg_queue_create failed for worker %d", i);
            return false;
        }

        ctx->ipc_threads[i] = ipc_thread_ctx_create(i, ctx->worker_pool,
                                                     ctx->ipc_cmd_queues[i],
                                                     ctx->ipc_ret_queues[i],
                                                     &ctx->main_cond);
        if (!ctx->ipc_threads[i]) {
            log_fatal("ipc_thread_ctx_create failed for worker %d", i);
            return false;
        }

        if (pthread_create(&ctx->ipc_tids[i], NULL, ipc_thread_loop, ctx->ipc_threads[i]) != 0) {
            log_fatal("pthread_create failed for IPC thread %d", i);
            return false;
        }
    }

    return true;
}

void destroy_ipc_threads(AppContext *ctx) {
    int n = ctx->worker_pool ? ctx->worker_pool->num_workers : 0;
    for (int i = 0; i < n; i++) {
        if (ctx->ipc_threads && ctx->ipc_threads[i]) {
            ipc_thread_stop(ctx->ipc_threads[i]);
        }
        if (ctx->ipc_tids) {
            pthread_join(ctx->ipc_tids[i], NULL);
        }
        if (ctx->ipc_threads && ctx->ipc_threads[i]) {
            ipc_thread_ctx_destroy(ctx->ipc_threads[i]);
        }
        if (ctx->ipc_cmd_queues && ctx->ipc_cmd_queues[i]) {
            msg_queue_destroy(ctx->ipc_cmd_queues[i]);
        }
        if (ctx->ipc_ret_queues && ctx->ipc_ret_queues[i]) {
            msg_queue_destroy(ctx->ipc_ret_queues[i]);
        }
    }
    free(ctx->ipc_cmd_queues);
    free(ctx->ipc_ret_queues);
    free(ctx->ipc_threads);
    free(ctx->ipc_tids);
    pthread_mutex_destroy(&ctx->main_mutex);
    pthread_cond_destroy(&ctx->main_cond);
}

void stop_all_ipc_threads(AppContext *ctx) {
    int n = ctx->worker_pool ? ctx->worker_pool->num_workers : 0;
    for (int i = 0; i < n; i++) {
        send_stop_to_ipc(ctx, i);
    }
}

/* ================================================================
 * wait_for_ipc_messages: cond_wait with timeout
 * ================================================================ */

static void wait_for_ipc_messages(AppContext *ctx, int timeout_ms) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (timeout_ms % 1000) * 1000000;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&ctx->main_mutex);
    pthread_cond_timedwait(&ctx->main_cond, &ctx->main_mutex, &ts);
    pthread_mutex_unlock(&ctx->main_mutex);
}

/* ================================================================
 * Main loop: Message Bus (v13.0.0) — pthread_cond_wait, no epoll
 * ================================================================ */

void main_loop_run(AppContext *ctx) {
    /* Create thread pool completion eventfd */
    ctx->event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (ctx->event_fd < 0) {
        log_fatal("eventfd creation failed");
        return;
    }

    ctx->thread_pool = thread_pool_create(ctx->cfg.master_threads, ctx->event_fd,
                                          batch_dedup_worker, ctx);
    if (!ctx->thread_pool) {
        log_fatal("Thread pool creation failed");
        close(ctx->event_fd);
        ctx->event_fd = -1;
        return;
    }

    ctx->running = true;

    while (ctx->running) {
        /* 1. Block on cond_wait for IPC messages (100ms timeout) */
        wait_for_ipc_messages(ctx, 100);

        /* v15.0.2 debug: 每 ~10s 打印一次主循环状态 */
        static int loop_counter = 0;
        if (++loop_counter >= 100) {
            loop_counter = 0;
            log_info("[MainLoop] pending_tasks=%ld pending_batches=%ld hist_state=%d lost_tasks=%zu",
                     atomic_load(&ctx->pending_tasks), atomic_load(&ctx->pending_batches),
                     ctx->hist_pump_state, ctx->lost_tasks.count);
        }

        /* 2. Drain all IPC return queues */
        for (int i = 0; i < ctx->worker_pool->num_workers; i++) {
            pthread_mutex_lock(&ctx->ipc_ret_queues[i]->mutex);
            size_t head = ctx->ipc_ret_queues[i]->head;
            size_t tail = ctx->ipc_ret_queues[i]->tail;
            pthread_mutex_unlock(&ctx->ipc_ret_queues[i]->mutex);
            log_debug("[Main] ret_queue[%d] head=%zu tail=%zu queue=%p", i, head, tail, (void*)ctx->ipc_ret_queues[i]);
            IpcThreadMsg msg;
            int drained = 0;
            while (msg_queue_recv(ctx->ipc_ret_queues[i], &msg)) {
                handle_return_message(ctx, &msg);
                drained++;
            }
            if (drained > 0) {
                log_debug("[Main] Drained %d messages from ret_queue[%d]", drained, i);
            }
        }

        /* 3. Drain thread pool completed batches */
        drain_completed_batches(ctx);
        /* Also drain eventfd counter to avoid stale notifications */
        if (ctx->event_fd >= 0) {
            uint64_t n;
            while (read(ctx->event_fd, &n, sizeof(n)) > 0) {
                drain_completed_batches(ctx);
            }
        }

        /* 4. Pump historical pbin directories */
        if (ctx->hist_pump_state == HIST_PUMP_OLD || ctx->hist_pump_state == HIST_PUMP_NEW) {
            pump_pbin_batch(ctx, ctx->cfg.batch_size);
        }

        /* 5. Reap zombie children */
        for (int i = 0; i < ctx->worker_pool->num_workers * 2; i++) {
            if (waitpid(-1, NULL, WNOHANG) <= 0) break;
        }

        /* 6. Replace dead workers */
        for (int i = 0; i < ctx->worker_pool->num_workers; i++) {
            WorkerSlot *slot = &ctx->worker_pool->slots[i];
            if (!atomic_load(&slot->is_alive) && slot->pid == -1) {
                cleanup_dead_worker_slot(ctx, i, true);
                log_info("[Replace] Replacing dead worker %d", i);
                worker_pool_replace(ctx->worker_pool, i);
                send_replace_to_ipc(ctx, i, slot->fd_cmd, slot->fd_data, slot->fd_ctrl, slot->pid);
            }
        }

        /* 7. Dispatch lost tasks */
        dispatch_lost_tasks(ctx);

        /* 8. Termination check */
        if (atomic_load(&ctx->pending_tasks) == 0 && !ctx->resume_active
            && atomic_load(&ctx->pending_batches) == 0) {
            worker_pool_stop_all(ctx->worker_pool);
            stop_all_ipc_threads(ctx);
            ctx->running = false;
        }
    }

    drain_completed_batches(ctx);
    record_path_batch_flush(&ctx->cfg, &ctx->state, &ctx->record_batch);

    thread_pool_destroy(ctx->thread_pool);
    ctx->thread_pool = NULL;
    close(ctx->event_fd);
    ctx->event_fd = -1;
    destroy_ipc_threads(ctx);
}

/**
 * @file main_loop.c
 * @brief 主消息总线与调度循环框架
 *
 * 负责：
 * - IPC 返回消息路由（handle_return_message）
 * - IPC 线程生命周期管理（init/destroy/stop）
 * - 主循环：cond_wait → drain ret_queue → drain batches → pump pbin → reap zombies → replace dead → dispatch lost → check termination
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
#include "lost_tasks.h"
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
 * Handle return messages from IPC threads
 * ================================================================ */

static void handle_return_message(AppContext *ctx, IpcThreadMsg *msg) {
    log_debug("[Bus] received type=%u slot=%d len=%zu", msg->type, msg->slot_id, msg->data_len);
    switch (msg->type) {
        case RET_BATCH: {
            log_debug_v(202605150000, "[Bus] Worker %d BATCH (len=%zu)", msg->slot_id, msg->data_len);
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
            log_info_v(202605150000, "[Bus] Worker %d FINISH (pending_tasks=%ld)", msg->slot_id, atomic_load(&ctx->pending_tasks));
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

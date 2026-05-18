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
#include "lost_tasks.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include <stdatomic.h>

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
 * Dispatch lost tasks (v13.0.0: send via cmd_queue)
 * ================================================================ */

void dispatch_lost_tasks(AppContext *ctx) {
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

        int wid = dispatch_find_idle_worker(ctx);
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
        log_debug_v(202605150000, "[LostTasks] dispatched %s to worker %d, pending_tasks=%ld", path_log_mask(path), wid, atomic_load(&ctx->pending_tasks));

        slot->current_dev = 0;
        safe_strcpy(slot->current_path, path, sizeof(slot->current_path));
        free(path);
    }
    lost_tasks_compact(&ctx->lost_tasks);
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
            log_debug_v(202605150000, "[Cleanup] Worker %d drained %d orphaned tasks from fd_cmd_rd", worker_id, orphaned);
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

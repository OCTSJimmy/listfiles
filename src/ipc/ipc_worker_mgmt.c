/**
 * @file ipc_worker_mgmt.c
 * @brief IPC Worker 生命周期管理
 *
 * 负责 IPC 线程中的 Worker 状态管理：
 * - Worker 死亡标记与 fd 清理（worker_mark_dead）
 * - 心跳超时杀掉（worker_timeout_kill）
 * - 向 Master 线程发送返回消息（send_return）
 */
#define _GNU_SOURCE
#include "ipc_thread.h"
#include "msg_format.h"
#include "ipc_protocol.h"
#include "log.h"
#include "utils.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <sys/epoll.h>

/* ================================================================
 * Worker death handling inside IPC thread
 * ================================================================ */

void worker_mark_dead(IpcThreadCtx *ctx, bool send_notify) {
    /* v15.6.1（P0-101）：捕获死亡 pid/epoch 供 Master 同代校验（须在清空 ctx->pid 前） */
    pid_t dead_pid = ctx->pid;
    uint64_t dead_epoch = ctx->current_epoch;

    if (ctx->fd_cmd >= 0) {
        close(ctx->fd_cmd);
        ctx->fd_cmd = -1;
    }
    if (ctx->fd_data >= 0) {
        if (ctx->epfd >= 0) {
            epoll_ctl(ctx->epfd, EPOLL_CTL_DEL, ctx->fd_data, NULL);
        }
        close(ctx->fd_data);
        ctx->fd_data = -1;
    }
    if (ctx->fd_ctrl >= 0) {
        if (ctx->epfd >= 0) {
            epoll_ctl(ctx->epfd, EPOLL_CTL_DEL, ctx->fd_ctrl, NULL);
        }
        close(ctx->fd_ctrl);
        ctx->fd_ctrl = -1;
    }
    ctx->pid = -1;
    atomic_store(&ctx->waiting_replace, true);

    if (send_notify) {
        RetDeathPayload *pl = malloc(sizeof(*pl));
        if (!pl) {
            /* v15.6.1: DEAD 丢失会导致 Master 永不替换/销账——不得静默 */
            log_fatal("[IPC-%d] malloc failed for DEAD payload (slot=%d)", ctx->slot_id, ctx->slot_id);
            return;
        }
        pl->reported_pid = dead_pid;
        pl->epoch = dead_epoch;
        IpcThreadMsg msg = {
            .type = RET_DEAD,
            .slot_id = ctx->slot_id,
            .data = pl,
            .data_len = sizeof(*pl),
            .epoch = dead_epoch
        };
        /* v15.6.0: 容量 65536，满即设计外异常——RET_DEAD 丢失会导致
         * Master 永不替换 Worker，log_fatal 暴露，不得静默丢弃 */
        if (!msg_queue_send(ctx->ret_queue, &msg)) {
            log_fatal("[IPC-%d] ret_queue full, DEAD message dropped (slot=%d)", ctx->slot_id, ctx->slot_id);
        }
    }
}

void worker_timeout_kill(IpcThreadCtx *ctx) {
    log_error("[IPC-%d] Worker %d heartbeat timeout, sending SIGKILL (pid=%d)",
            ctx->slot_id, ctx->slot_id, (int)ctx->pid);
    if (ctx->pid > 0) {
        kill(ctx->pid, SIGKILL);
    }
    worker_mark_dead(ctx, true);
}

/* ================================================================
 * Send return message to master
 * ================================================================ */

void send_return(IpcThreadCtx *ctx, uint32_t type, void *data, size_t len, uint64_t epoch) {
    IpcThreadMsg msg = {
        .type = type,
        .slot_id = ctx->slot_id,
        .data = data,
        .data_len = len,
        .epoch = epoch
    };
    /* v15.6.0: 容量 65536，满即设计外异常——RET_BATCH/RET_FINISH 丢失会导致
     * pending_tasks 永久泄漏、完结检查卡死，log_fatal 暴露，不得静默丢弃 */
    if (!msg_queue_send(ctx->ret_queue, &msg)) {
        log_fatal("[IPC-%d] ret_queue full, message type=%u slot=%d dropped", ctx->slot_id, type, ctx->slot_id);
        free(data);
    } else {
        log_info_v(202605150000UL, "[IPC-%d] ret_queue send OK (type=%u, len=%zu, queue=%p)", ctx->slot_id, type, len, (void*)ctx->ret_queue);
        if (ctx->master_cond) {
            pthread_cond_signal(ctx->master_cond);
        }
    }
}

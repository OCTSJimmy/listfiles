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
        IpcThreadMsg msg = {
            .type = RET_DEAD,
            .slot_id = ctx->slot_id,
            .data = NULL,
            .data_len = 0
        };
        if (!msg_queue_send(ctx->ret_queue, &msg)) {
            log_error("[IPC-%d] ret_queue full, DEAD message dropped", ctx->slot_id);
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

void send_return(IpcThreadCtx *ctx, uint32_t type, void *data, size_t len) {
    IpcThreadMsg msg = {
        .type = type,
        .slot_id = ctx->slot_id,
        .data = data,
        .data_len = len
    };
    if (!msg_queue_send(ctx->ret_queue, &msg)) {
        log_error("[IPC-%d] ret_queue full, message type=%u dropped", ctx->slot_id, type);
        free(data);
    } else {
        log_info("[IPC-%d] ret_queue send OK (type=%u, len=%zu, queue=%p)", ctx->slot_id, type, len, (void*)ctx->ret_queue);
        if (ctx->master_cond) {
            pthread_cond_signal(ctx->master_cond);
        }
    }
}

/**
 * @file ipc_thread.c
 * @brief IPC 线程生命周期与 epoll 主循环
 *
 * 负责 IPC 线程的上下文创建销毁，以及 epoll 事件驱动主循环：
 * - 线程上下文创建/销毁（ipc_thread_ctx_create / ipc_thread_ctx_destroy）
 * - epoll 主循环：cmd_queue eventfd + fd_data + fd_ctrl 事件分发（ipc_thread_loop）
 * - 心跳超时检测与 Worker 杀掉
 * - 线程停止信号（ipc_thread_stop）
 */
#define _GNU_SOURCE
#include "ipc_thread.h"
#include "msg_format.h"
#include "log.h"
#include "utils.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <signal.h>

/* ================================================================
 * Public API
 * ================================================================ */

IpcThreadCtx* ipc_thread_ctx_create(int slot_id, WorkerPool *pool,
                                   MsgQueue *cmd, MsgQueue *ret,
                                   pthread_cond_t *master_cond) {
    IpcThreadCtx *ctx = calloc(1, sizeof(IpcThreadCtx));
    if (!ctx) return NULL;

    ctx->slot_id = slot_id;
    ctx->pool = pool;
    ctx->cmd_queue = cmd;
    ctx->ret_queue = ret;
    ctx->master_cond = master_cond;
    ctx->fd_cmd = -1;
    ctx->fd_data = -1;
    ctx->fd_ctrl = -1;
    ctx->pid = -1;
    atomic_init(&ctx->running, true);
    atomic_init(&ctx->last_heartbeat, time(NULL));
    atomic_init(&ctx->waiting_replace, false);
    ctx->eagain_retry_count = 0;

    ctx->epfd = epoll_create1(EPOLL_CLOEXEC);
    if (ctx->epfd < 0) {
        log_error("[IPC-%d] epoll_create1 failed: %s", slot_id, strerror(errno));
        free(ctx);
        return NULL;
    }

    /* Add cmd_queue eventfd to epoll */
    if (cmd && cmd->eventfd >= 0) {
        struct epoll_event ev = {0};
        ev.events = EPOLLIN;
        ev.data.u32 = 1; /* slot 1 = cmd_queue eventfd */
        if (epoll_ctl(ctx->epfd, EPOLL_CTL_ADD, cmd->eventfd, &ev) != 0) {
            log_error("[IPC-%d] epoll_ctl ADD cmd_queue eventfd failed: %s",
                    slot_id, strerror(errno));
        }
    }

    return ctx;
}

void ipc_thread_ctx_destroy(IpcThreadCtx *ctx) {
    if (!ctx) return;
    if (ctx->fd_cmd >= 0) close(ctx->fd_cmd);
    if (ctx->fd_data >= 0) close(ctx->fd_data);
    if (ctx->fd_ctrl >= 0) close(ctx->fd_ctrl);
    if (ctx->epfd >= 0) close(ctx->epfd);
    free(ctx);
}

void* ipc_thread_loop(void *arg) {
    IpcThreadCtx *ctx = (IpcThreadCtx*)arg;
    if (!ctx) return NULL;

    struct epoll_event events[8];

    while (atomic_load(&ctx->running)) {
        /* 1. Handle commands (non-blocking drain) */
        IpcThreadMsg cmd;
        while (msg_queue_recv(ctx->cmd_queue, &cmd)) {
            handle_cmd(ctx, &cmd);
        }

        /* 2. epoll_wait: fd_data + fd_ctrl + cmd_queue eventfd */
        int nfds = epoll_wait(ctx->epfd, events, 8, 500);

        for (int i = 0; i < nfds; i++) {
            uint32_t slot = events[i].data.u32;

            if (slot == 1) {
                /* cmd_queue eventfd: drain and process commands */
                msg_queue_drain_eventfd(ctx->cmd_queue);
                while (msg_queue_recv(ctx->cmd_queue, &cmd)) {
                    handle_cmd(ctx, &cmd);
                }
                continue;
            }

            if (slot == 2) {
                /* fd_data event: BATCH data */
                if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                    log_error("[IPC-%d] fd_data error/hup (events=0x%x)",
                            ctx->slot_id, events[i].events);
                    worker_mark_dead(ctx, true);
                    continue;
                }
                read_data_message(ctx);
                continue;
            }

            if (slot == 3) {
                /* fd_ctrl event: control signals */
                if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                    log_error("[IPC-%d] fd_ctrl error/hup (events=0x%x)",
                            ctx->slot_id, events[i].events);
                    worker_mark_dead(ctx, true);
                    continue;
                }
                read_ctrl_message(ctx);
                continue;
            }
        }

        /* 3. Heartbeat timeout check */
        if (!atomic_load(&ctx->waiting_replace) && ctx->pid > 0) {
            time_t now = time(NULL);
            time_t last = atomic_load(&ctx->last_heartbeat);
            /* v15.1.1: startup_timeout=60s for INITIALIZING workers, normal 30s after READY/HEARTBEAT */
            int timeout_sec = (last == ctx->spawn_time) ? 60 : HEARTBEAT_TIMEOUT_SEC;
            if (difftime(now, last) > timeout_sec) {
                log_warn("[IPC-%d] heartbeat timeout (last=%ld, spawn=%ld, timeout=%ds), killing worker",
                        ctx->slot_id, (long)last, (long)ctx->spawn_time, timeout_sec);
                worker_timeout_kill(ctx);
            }
        }
    }

    return NULL;
}

void ipc_thread_stop(IpcThreadCtx *ctx) {
    if (!ctx) return;
    atomic_store(&ctx->running, false);
}

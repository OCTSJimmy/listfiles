#define _GNU_SOURCE
#include "ipc_thread.h"
#include "msg_format.h"
#include "ipc_protocol.h"
#include "log.h"
#include "utils.h"
#include "worker_proc.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <signal.h>
#include <poll.h>

/* ================================================================
 * IPC Thread Implementation (v13.0.0)
 * Each thread manages one Worker: non-blocking epoll + heartbeat.
 * ================================================================ */

static int safe_ipc_recv_header(int fd, IpcMessageHeader *hdr) {
    struct pollfd pfd = { fd, POLLIN, 0 };
    int rc = poll(&pfd, 1, 100);
    if (rc == 0) return -2;
    if (rc < 0) {
        if (errno == EINTR) return -2;
        log_error("[IPC] poll error on fd=%d: errno=%d (%s)",
                fd, errno, strerror(errno));
        return -1;
    }
    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
        log_error("[IPC] poll error/hup on fd=%d (revents=0x%x)",
                fd, pfd.revents);
        return -1;
    }
    return ipc_recv_header(fd, hdr);
}

static int safe_ipc_recv_payload(int fd, void *buf, uint32_t len) {
    if (len == 0) return 0;
    size_t nread = 0;
    while (nread < len) {
        struct pollfd pfd = { fd, POLLIN, 0 };
        int rc = poll(&pfd, 1, 100);
        if (rc == 0) return -2;
        if (rc < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) return -1;

        ssize_t n = read(fd, (char*)buf + nread, len - nread);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return -2;
            return -1;
        }
        if (n == 0) return -1;
        nread += n;
    }
    return 0;
}

/* ---- Worker death handling inside IPC thread ---- */
static void worker_mark_dead(IpcThreadCtx *ctx, bool send_notify) {
    if (ctx->fd_in >= 0) {
        close(ctx->fd_in);
        ctx->fd_in = -1;
    }
    if (ctx->fd_out >= 0) {
        if (ctx->epfd >= 0) {
            epoll_ctl(ctx->epfd, EPOLL_CTL_DEL, ctx->fd_out, NULL);
        }
        close(ctx->fd_out);
        ctx->fd_out = -1;
    }
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

static void worker_timeout_kill(IpcThreadCtx *ctx) {
    log_error("[IPC-%d] Worker %d heartbeat timeout, sending SIGKILL (pid=%d)",
            ctx->slot_id, ctx->slot_id, (int)ctx->pid);
    if (ctx->pid > 0) {
        kill(ctx->pid, SIGKILL);
    }
    worker_mark_dead(ctx, true);
}

/* ---- Send return message to master ---- */
static void send_return(IpcThreadCtx *ctx, uint32_t type, void *data, size_t len) {
    IpcThreadMsg msg = {
        .type = type,
        .slot_id = ctx->slot_id,
        .data = data,
        .data_len = len
    };
    if (!msg_queue_send(ctx->ret_queue, &msg)) {
        log_error("[IPC-%d] ret_queue full, message type=%u dropped", ctx->slot_id, type);
        free(data);
    } else if (ctx->master_cond) {
        pthread_cond_signal(ctx->master_cond);
    }
}

/* ---- Read a complete IPC message from Worker fd_out ---- */
static void read_worker_message(IpcThreadCtx *ctx) {
    IpcMessageHeader hdr;
    int rc_hdr = safe_ipc_recv_header(ctx->fd_out, &hdr);
    if (rc_hdr == -2) {
        return; /* timeout, try next loop */
    }
    if (rc_hdr != 0) {
        log_error("[IPC-%d] recv_header failed, marking worker dead", ctx->slot_id);
        worker_mark_dead(ctx, true);
        return;
    }

    if (hdr.payload_len > 16 * 1024 * 1024) {
        void *tmp = malloc(hdr.payload_len);
        if (tmp) { safe_ipc_recv_payload(ctx->fd_out, tmp, hdr.payload_len); free(tmp); }
        return;
    }

    void *payload = NULL;
    if (hdr.payload_len > 0) {
        payload = malloc(hdr.payload_len);
        int rc_payload = safe_ipc_recv_payload(ctx->fd_out, payload, hdr.payload_len);
        if (rc_payload != 0) {
            free(payload);
            if (rc_payload == -2) {
                log_warn("[IPC-%d] payload timeout (len=%u)", ctx->slot_id, hdr.payload_len);
            } else {
                log_error("[IPC-%d] payload recv failed, marking worker dead", ctx->slot_id);
                worker_mark_dead(ctx, true);
            }
            return;
        }
    }

    switch (hdr.msg_type) {
        case IPC_MSG_BATCH: {
            send_return(ctx, RET_BATCH, payload, hdr.payload_len);
            payload = NULL; /* ownership transferred */
            break;
        }
        case IPC_MSG_HEARTBEAT: {
            if (hdr.payload_len >= sizeof(IpcHeartbeatPayload)) {
                IpcHeartbeatPayload *hb = (IpcHeartbeatPayload*)payload;
                atomic_store(&ctx->last_heartbeat, (time_t)hb->timestamp);

                /* Forward heartbeat to master for Monitor display */
                RetHeartbeatPayload *ret = malloc(sizeof(RetHeartbeatPayload));
                if (ret) {
                    ret->timestamp = hb->timestamp;
                    send_return(ctx, RET_HEARTBEAT, ret, sizeof(*ret));
                }
            }
            free(payload);
            break;
        }
        case IPC_MSG_ERROR: {
            if (hdr.payload_len >= sizeof(IpcErrorHeader)) {
                IpcErrorHeader *eh = (IpcErrorHeader*)payload;
                RetErrorPayload *ret = malloc(sizeof(RetErrorPayload));
                if (ret) {
                    ret->errno_code = eh->errno_code;
                    ret->dev = eh->dev;
                    /* path follows IpcErrorHeader + uint32_t path_len */
                    if (hdr.payload_len > sizeof(IpcErrorHeader) + sizeof(uint32_t)) {
                        const char *src = (const char*)payload + sizeof(IpcErrorHeader) + sizeof(uint32_t);
                        size_t plen = hdr.payload_len - sizeof(IpcErrorHeader) - sizeof(uint32_t);
                        if (plen >= sizeof(ret->path)) plen = sizeof(ret->path) - 1;
                        memcpy(ret->path, src, plen);
                        ret->path[plen] = '\0';
                    } else {
                        ret->path[0] = '\0';
                    }
                    send_return(ctx, RET_ERROR, ret, sizeof(*ret));
                }
            }
            free(payload);
            break;
        }
        case IPC_MSG_EXIT: {
            send_return(ctx, RET_EXIT, NULL, 0);
            worker_mark_dead(ctx, false); /* fd already closed by worker */
            break;
        }
        default:
            free(payload);
            break;
    }
}

/* ---- Handle commands from master thread ---- */
static void handle_cmd(IpcThreadCtx *ctx, IpcThreadMsg *cmd) {
    switch (cmd->type) {
        case CMD_SCAN: {
            CmdScanPayload *scan = (CmdScanPayload*)cmd->data;
            if (!scan) break;
            if (ctx->fd_in < 0) {
                /* Replacement 窗口期：fd_in 尚未就绪，通知 Master 重入队 */
                DropPayload *drop = malloc(sizeof(DropPayload));
                if (drop) {
                    safe_strcpy(drop->path, scan->path, sizeof(drop->path));
                    IpcThreadMsg drop_msg = {
                        .type = MSG_DROP,
                        .slot_id = ctx->slot_id,
                        .data = drop,
                        .data_len = sizeof(*drop)
                    };
                    if (!msg_queue_send(ctx->ret_queue, &drop_msg)) {
                        log_warn("[IPC-%d] MSG_DROP send failed, leaking path", ctx->slot_id);
                        free(drop);
                    } else if (ctx->master_cond) {
                        pthread_cond_signal(ctx->master_cond);
                    }
                }
                break;
            }
            static _Thread_local int eagain_retry_count = 0;
            int rc = ipc_send(ctx->fd_in, IPC_MSG_SCAN, scan->path, scan->path_len);
            if (rc == -2) {
                /* EAGAIN */
                eagain_retry_count++;
                if (eagain_retry_count > 10) {
                    log_error("[IPC-%d] ipc_send EAGAIN exhausted (%d retries), marking worker dead",
                              ctx->slot_id, eagain_retry_count);
                    worker_mark_dead(ctx, true);
                    eagain_retry_count = 0;
                    break;
                }
                /* push back to queue for retry */
                if (!msg_queue_send(ctx->cmd_queue, cmd)) {
                    log_warn("[IPC-%d] CMD_SCAN EAGAIN, cmd_queue full, dropping %s",
                            ctx->slot_id, scan->path);
                } else {
                    cmd->data = NULL; /* prevent double free */
                }
            } else if (rc == -1) {
                log_error("[IPC-%d] CMD_SCAN ipc_send failed, marking worker dead", ctx->slot_id);
                eagain_retry_count = 0;
                worker_mark_dead(ctx, true);
            } else {
                eagain_retry_count = 0;
            }
            break;
        }
        case CMD_REPLACE: {
            CmdReplacePayload *rep = (CmdReplacePayload*)cmd->data;
            if (!rep) break;

            /* Close old fds */
            if (ctx->fd_in >= 0) { close(ctx->fd_in); ctx->fd_in = -1; }
            if (ctx->fd_out >= 0) {
                if (ctx->epfd >= 0) epoll_ctl(ctx->epfd, EPOLL_CTL_DEL, ctx->fd_out, NULL);
                close(ctx->fd_out);
                ctx->fd_out = -1;
            }

            /* Set new fds */
            ctx->fd_in = rep->fd_in;
            ctx->fd_out = rep->fd_out;
            ctx->pid = rep->pid;
            atomic_store(&ctx->last_heartbeat, time(NULL));
            atomic_store(&ctx->waiting_replace, false);

            /* Add new fd_out to epoll */
            if (ctx->epfd >= 0 && ctx->fd_out >= 0) {
                struct epoll_event ev = {0};
                ev.events = EPOLLIN | EPOLLERR | EPOLLHUP;
                ev.data.u32 = 0; /* slot 0 = fd_out */
                if (epoll_ctl(ctx->epfd, EPOLL_CTL_ADD, ctx->fd_out, &ev) != 0) {
                    log_error("[IPC-%d] epoll_ctl ADD fd_out=%d failed: %s",
                            ctx->slot_id, ctx->fd_out, strerror(errno));
                    worker_mark_dead(ctx, true);
                }
            }
            log_info("[IPC-%d] Worker replaced (pid=%d, fd_out=%d)",
                    ctx->slot_id, (int)ctx->pid, ctx->fd_out);
            break;
        }
        case CMD_STOP: {
            atomic_store(&ctx->running, false);
            break;
        }
    }
    free(cmd->data);
    cmd->data = NULL;
}

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
    ctx->fd_in = -1;
    ctx->fd_out = -1;
    ctx->pid = -1;
    atomic_init(&ctx->running, true);
    atomic_init(&ctx->last_heartbeat, time(NULL));
    atomic_init(&ctx->waiting_replace, false);

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
    if (ctx->fd_in >= 0) close(ctx->fd_in);
    if (ctx->fd_out >= 0) close(ctx->fd_out);
    if (ctx->epfd >= 0) close(ctx->epfd);
    free(ctx);
}

void* ipc_thread_loop(void *arg) {
    IpcThreadCtx *ctx = (IpcThreadCtx*)arg;
    if (!ctx) return NULL;

    struct epoll_event events[4];

    while (atomic_load(&ctx->running)) {
        /* 1. Handle commands (non-blocking drain) */
        IpcThreadMsg cmd;
        while (msg_queue_recv(ctx->cmd_queue, &cmd)) {
            handle_cmd(ctx, &cmd);
        }

        /* 2. epoll_wait: fd_out + cmd_queue eventfd */
        int nfds = epoll_wait(ctx->epfd, events, 4, 500);

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

            if (slot == 0) {
                /* fd_out event */
                if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                    log_error("[IPC-%d] fd_out error/hup (events=0x%x)",
                            ctx->slot_id, events[i].events);
                    worker_mark_dead(ctx, true);
                    continue;
                }
                read_worker_message(ctx);
            }
        }

        /* 3. Heartbeat timeout check */
        if (!atomic_load(&ctx->waiting_replace) && ctx->pid > 0) {
            time_t now = time(NULL);
            time_t last = atomic_load(&ctx->last_heartbeat);
            if (difftime(now, last) > HEARTBEAT_TIMEOUT_SEC) {
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

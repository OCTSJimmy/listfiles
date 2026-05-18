/**
 * @file ipc_message_handler.c
 * @brief IPC 消息接收与处理
 *
 * 负责 IPC 线程中的消息安全接收与协议处理：
 * - 带 poll 超时的 IPC 安全接收（safe_ipc_recv_header / safe_ipc_recv_payload）
 * - 控制消息读取：HEARTBEAT / ERROR / DEV_TIMEOUT / READY / FINISH / EXIT（read_ctrl_message）
 * - 数据消息读取：BATCH 数据转发（read_data_message）
 * - 主线程命令处理：CMD_SCAN / CMD_REPLACE / CMD_STOP（handle_cmd）
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
#include <sys/epoll.h>
#include <poll.h>

/* ================================================================
 * Safe IPC receive helpers (static — only used within this file)
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

/* ================================================================
 * Read a complete IPC message from Worker fd_ctrl
 * ================================================================ */

void read_ctrl_message(IpcThreadCtx *ctx) {
    IpcMessageHeader hdr;
    int rc_hdr = safe_ipc_recv_header(ctx->fd_ctrl, &hdr);
    if (rc_hdr == -2) {
        return; /* timeout, try next loop */
    }
    if (rc_hdr != 0) {
        log_error("[IPC-%d] recv_header failed on fd_ctrl, marking worker dead", ctx->slot_id);
        worker_mark_dead(ctx, true);
        return;
    }

    if (hdr.payload_len > 16 * 1024 * 1024) {
        void *tmp = malloc(hdr.payload_len);
        if (tmp) { safe_ipc_recv_payload(ctx->fd_ctrl, tmp, hdr.payload_len); free(tmp); }
        return;
    }

    void *payload = NULL;
    if (hdr.payload_len > 0) {
        payload = malloc(hdr.payload_len);
        int rc_payload = safe_ipc_recv_payload(ctx->fd_ctrl, payload, hdr.payload_len);
        if (rc_payload != 0) {
            free(payload);
            if (rc_payload == -2) {
                log_warn("[IPC-%d] ctrl payload timeout (len=%u)", ctx->slot_id, hdr.payload_len);
            } else {
                log_error("[IPC-%d] ctrl payload recv failed, marking worker dead", ctx->slot_id);
                worker_mark_dead(ctx, true);
            }
            return;
        }
    }

    switch (hdr.msg_type) {
        case IPC_MSG_HEARTBEAT: {
            if (hdr.payload_len >= sizeof(IpcHeartbeatPayload)) {
                IpcHeartbeatPayload *hb = (IpcHeartbeatPayload*)payload;
                atomic_store(&ctx->last_heartbeat, (time_t)hb->timestamp);

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
        case IPC_MSG_DEV_TIMEOUT: {
            if (hdr.payload_len >= sizeof(IpcErrorHeader)) {
                IpcErrorHeader *eh = (IpcErrorHeader*)payload;
                RetErrorPayload *ret = malloc(sizeof(RetErrorPayload));
                if (ret) {
                    ret->errno_code = eh->errno_code;
                    ret->dev = eh->dev;
                    if (hdr.payload_len > sizeof(IpcErrorHeader) + sizeof(uint32_t)) {
                        const char *src = (const char*)payload + sizeof(IpcErrorHeader) + sizeof(uint32_t);
                        size_t plen = hdr.payload_len - sizeof(IpcErrorHeader) - sizeof(uint32_t);
                        if (plen >= sizeof(ret->path)) plen = sizeof(ret->path) - 1;
                        memcpy(ret->path, src, plen);
                        ret->path[plen] = '\0';
                    } else {
                        ret->path[0] = '\0';
                    }
                    send_return(ctx, RET_DEV_TIMEOUT, ret, sizeof(*ret));
                }
            }
            free(payload);
            break;
        }
        case IPC_MSG_READY: {
            log_debug_v(202605150000, "[IPC-%d] received READY, forwarding RET_READY", ctx->slot_id);
            send_return(ctx, RET_READY, NULL, 0);
            free(payload);
            break;
        }
        case IPC_MSG_FINISH: {
            if (hdr.payload_len >= sizeof(IpcFinishPayload)) {
                IpcFinishPayload *fin = (IpcFinishPayload*)payload;
                log_info("[IPC-%d] received FINISH (path_len=%u), forwarding RET_FINISH", ctx->slot_id, fin->path_len);
                /* Payload: IpcFinishPayload + path bytes */
                size_t path_len = fin->path_len;
                if (path_len > 4095) path_len = 4095;
                char *path_buf = malloc(path_len + 1);
                if (path_buf) {
                    if (hdr.payload_len >= sizeof(IpcFinishPayload) + path_len) {
                        memcpy(path_buf, (char*)payload + sizeof(IpcFinishPayload), path_len);
                    }
                    path_buf[path_len] = '\0';
                    send_return(ctx, RET_FINISH, path_buf, path_len + 1);
                }
            }
            free(payload);
            break;
        }
        case IPC_MSG_EXIT: {
            send_return(ctx, RET_EXIT, NULL, 0);
            worker_mark_dead(ctx, false);
            break;
        }
        default:
            free(payload);
            break;
    }
}

/* ================================================================
 * Read a complete BATCH message from Worker fd_data
 * ================================================================ */

void read_data_message(IpcThreadCtx *ctx) {
    IpcMessageHeader hdr;
    int rc_hdr = safe_ipc_recv_header(ctx->fd_data, &hdr);
    if (rc_hdr == -2) {
        return; /* timeout, try next loop */
    }
    if (rc_hdr != 0) {
        log_error("[IPC-%d] recv_header failed on fd_data, marking worker dead", ctx->slot_id);
        worker_mark_dead(ctx, true);
        return;
    }

    if (hdr.msg_type != IPC_MSG_BATCH) {
        /* Unexpected message type on fd_data - discard */
        void *tmp = malloc(hdr.payload_len);
        if (tmp) { safe_ipc_recv_payload(ctx->fd_data, tmp, hdr.payload_len); free(tmp); }
        return;
    }

    if (hdr.payload_len > 16 * 1024 * 1024) {
        void *tmp = malloc(hdr.payload_len);
        if (tmp) { safe_ipc_recv_payload(ctx->fd_data, tmp, hdr.payload_len); free(tmp); }
        return;
    }

    void *payload = NULL;
    if (hdr.payload_len > 0) {
        payload = malloc(hdr.payload_len);
        int rc_payload = safe_ipc_recv_payload(ctx->fd_data, payload, hdr.payload_len);
        if (rc_payload != 0) {
            free(payload);
            if (rc_payload == -2) {
                log_warn("[IPC-%d] data payload timeout (len=%u)", ctx->slot_id, hdr.payload_len);
            } else {
                log_error("[IPC-%d] data payload recv failed, marking worker dead", ctx->slot_id);
                worker_mark_dead(ctx, true);
            }
            return;
        }
    }

    log_debug_v(202605150000, "[IPC-%d] received BATCH (payload=%u), forwarding RET_BATCH", ctx->slot_id, hdr.payload_len);
    send_return(ctx, RET_BATCH, payload, hdr.payload_len);
    /* ownership transferred */
}

/* ================================================================
 * Handle commands from master thread
 * ================================================================ */

void handle_cmd(IpcThreadCtx *ctx, IpcThreadMsg *cmd) {
    switch (cmd->type) {
        case CMD_SCAN: {
            CmdScanPayload *scan = (CmdScanPayload*)cmd->data;
            if (!scan) break;
            if (ctx->fd_cmd < 0) {
                /* Replacement 窗口期：fd_cmd 尚未就绪，通知 Master 重入队 */
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
            int rc = ipc_send(ctx->fd_cmd, IPC_MSG_SCAN, scan->path, scan->path_len);
            if (rc == -2) {
                /* EAGAIN */
                ctx->eagain_retry_count++;
                if (ctx->eagain_retry_count > 10) {
                    log_error("[IPC-%d] ipc_send EAGAIN exhausted (%d retries), marking worker dead",
                              ctx->slot_id, ctx->eagain_retry_count);
                    worker_mark_dead(ctx, true);
                    ctx->eagain_retry_count = 0;
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
                ctx->eagain_retry_count = 0;
                worker_mark_dead(ctx, true);
            } else {
                ctx->eagain_retry_count = 0;
                log_info_v(202605150000, "[IPC-%d] CMD_SCAN sent to worker (path=%s, len=%u)", ctx->slot_id, path_log_mask(scan->path), scan->path_len);
            }
            break;
        }
        case CMD_REPLACE: {
            CmdReplacePayload *rep = (CmdReplacePayload*)cmd->data;
            if (!rep) break;

            /* Close old fds */
            if (ctx->fd_cmd >= 0) { close(ctx->fd_cmd); ctx->fd_cmd = -1; }
            if (ctx->fd_data >= 0) {
                if (ctx->epfd >= 0) epoll_ctl(ctx->epfd, EPOLL_CTL_DEL, ctx->fd_data, NULL);
                close(ctx->fd_data);
                ctx->fd_data = -1;
            }
            if (ctx->fd_ctrl >= 0) {
                if (ctx->epfd >= 0) epoll_ctl(ctx->epfd, EPOLL_CTL_DEL, ctx->fd_ctrl, NULL);
                close(ctx->fd_ctrl);
                ctx->fd_ctrl = -1;
            }

            /* Set new fds */
            ctx->fd_cmd = rep->fd_cmd;
            ctx->fd_data = rep->fd_data;
            ctx->fd_ctrl = rep->fd_ctrl;
            ctx->pid = rep->pid;
            atomic_store(&ctx->last_heartbeat, time(NULL));
            ctx->spawn_time = time(NULL);  /* v15.1.1: record spawn time for startup_timeout */
            atomic_store(&ctx->waiting_replace, false);

            /* Add new fd_data to epoll */
            if (ctx->epfd >= 0 && ctx->fd_data >= 0) {
                struct epoll_event ev = {0};
                ev.events = EPOLLIN | EPOLLERR | EPOLLHUP;
                ev.data.u32 = 2; /* slot 2 = fd_data */
                if (epoll_ctl(ctx->epfd, EPOLL_CTL_ADD, ctx->fd_data, &ev) != 0) {
                    log_error("[IPC-%d] epoll_ctl ADD fd_data=%d failed: %s",
                            ctx->slot_id, ctx->fd_data, strerror(errno));
                    worker_mark_dead(ctx, true);
                }
            }
            /* Add new fd_ctrl to epoll */
            if (ctx->epfd >= 0 && ctx->fd_ctrl >= 0) {
                struct epoll_event ev = {0};
                ev.events = EPOLLIN | EPOLLERR | EPOLLHUP;
                ev.data.u32 = 3; /* slot 3 = fd_ctrl */
                if (epoll_ctl(ctx->epfd, EPOLL_CTL_ADD, ctx->fd_ctrl, &ev) != 0) {
                    log_error("[IPC-%d] epoll_ctl ADD fd_ctrl=%d failed: %s",
                            ctx->slot_id, ctx->fd_ctrl, strerror(errno));
                    worker_mark_dead(ctx, true);
                }
            }
            log_info("[IPC-%d] Worker replaced (pid=%d, fd_data=%d, fd_ctrl=%d)",
                    ctx->slot_id, (int)ctx->pid, ctx->fd_data, ctx->fd_ctrl);
            ctx->eagain_retry_count = 0;
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

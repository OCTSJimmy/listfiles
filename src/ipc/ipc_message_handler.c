/**
 * @file ipc_message_handler.c
 * @brief IPC 消息接收与处理 (v15.4.0 FSM)
 *
 * 负责 IPC 线程中的消息安全接收与协议处理：
 * - 跨 epoll 可续传的 FSM 读取（safe_ipc_recv_header_fsm / payload_fsm / footer_fsm）
 * - 控制消息读取：HEARTBEAT / ERROR / DEV_TIMEOUT / READY / FINISH / EXIT（read_ctrl_message）
 * - 数据消息读取：BATCH 数据 + Footer 魔数校验（read_data_message）
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

static void drain_fd(int fd) {
    uint8_t buf[4096];
    while (read(fd, buf, sizeof(buf)) > 0) {}
}

/* ================================================================
 * Read a complete IPC message from Worker fd_ctrl (v15.4.0 FSM)
 * ================================================================ */

void read_ctrl_message(IpcThreadCtx *ctx) {
    int rc;
    IpcReadFsm *fsm = &ctx->ctrl_fsm;

    /* --- HDR phase --- */
    if (fsm->state == IPC_READ_IDLE) {
        fsm->state = IPC_READ_HDR;
        fsm->nread = 0;
    }
    if (fsm->state == IPC_READ_HDR) {
        rc = safe_ipc_recv_header_fsm(ctx->fd_ctrl, fsm);
        if (rc == -2) return; /* EAGAIN, resume next epoll */
        if (rc != 0) {
            log_error("[IPC-%d] ctrl recv_header failed, marking worker dead", ctx->slot_id);
            worker_mark_dead(ctx, true);
            return;
        }
        /* Header complete — validate before allocating payload */
        if (!ipc_msg_type_valid(fsm->hdr.msg_type)) {
            log_error("[IPC-%d] ctrl garbage header (type=%u len=%u), draining fd",
                      ctx->slot_id, fsm->hdr.msg_type, fsm->hdr.payload_len);
            drain_fd(ctx->fd_ctrl);
            fsm->state = IPC_READ_IDLE;
            return;
        }
        if (fsm->hdr.payload_len > 16 * 1024 * 1024) {
            log_warn("[IPC-%d] ctrl payload_len %u suspicious, draining",
                     ctx->slot_id, fsm->hdr.payload_len);
            drain_fd(ctx->fd_ctrl);
            fsm->state = IPC_READ_IDLE;
            return;
        }
        if (fsm->hdr.payload_len == 0) {
            /* No payload — skip directly to dispatch */
            goto dispatch;
        }
        /* Allocate and enter PAYLOAD phase */
        fsm->buf = malloc(fsm->hdr.payload_len);
        if (!fsm->buf) {
            log_error("[IPC-%d] ctrl malloc(%u) failed, marking worker dead",
                      ctx->slot_id, fsm->hdr.payload_len);
            worker_mark_dead(ctx, true);
            return;
        }
        fsm->state = IPC_READ_PAYLOAD;
        fsm->nread = 0;
    }

    /* --- PAYLOAD phase --- */
    if (fsm->state == IPC_READ_PAYLOAD) {
        rc = safe_ipc_recv_payload_fsm(ctx->fd_ctrl, fsm);
        if (rc == -2) return; /* timeout, resume next epoll */
        if (rc != 0) {
            log_error("[IPC-%d] ctrl payload recv failed, marking worker dead", ctx->slot_id);
            free(fsm->buf); fsm->buf = NULL;
            worker_mark_dead(ctx, true);
            return;
        }
        /* Payload complete */
    }

dispatch:
    {
        void *payload = fsm->buf;
        IpcMessageHeader hdr = fsm->hdr;
        /* Reset FSM before dispatch so recursive calls won't confuse state */
        fsm->state = IPC_READ_IDLE;
        fsm->nread = 0;
        fsm->buf = NULL;

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
                free(payload);
                break;
            }
            default:
                free(payload);
                break;
        }
    }
}

/* ================================================================
 * Read a complete BATCH message from Worker fd_data (v15.4.0 FSM)
 * ================================================================ */

void read_data_message(IpcThreadCtx *ctx) {
    int rc;
    IpcReadFsm *fsm = &ctx->data_fsm;

    /* --- HDR phase --- */
    if (fsm->state == IPC_READ_IDLE) {
        fsm->state = IPC_READ_HDR;
        fsm->nread = 0;
    }
    if (fsm->state == IPC_READ_HDR) {
        rc = safe_ipc_recv_header_fsm(ctx->fd_data, fsm);
        if (rc == -2) return;
        if (rc != 0) {
            log_error("[IPC-%d] data recv_header failed, marking worker dead", ctx->slot_id);
            worker_mark_dead(ctx, true);
            return;
        }
        /* Validate */
        if (fsm->hdr.msg_type != IPC_MSG_BATCH) {
            log_error("[IPC-%d] data unexpected type=%u, draining fd", ctx->slot_id, fsm->hdr.msg_type);
            drain_fd(ctx->fd_data);
            fsm->state = IPC_READ_IDLE;
            return;
        }
        /* payload_len must cover at least Footer (8 bytes) */
        if (fsm->hdr.payload_len < sizeof(uint64_t)) {
            log_error("[IPC-%d] data payload_len %u < footer size, draining",
                      ctx->slot_id, fsm->hdr.payload_len);
            drain_fd(ctx->fd_data);
            fsm->state = IPC_READ_IDLE;
            return;
        }
        if (fsm->hdr.payload_len > 16 * 1024 * 1024 + sizeof(uint64_t)) {
            log_warn("[IPC-%d] data payload_len %u suspicious, draining",
                     ctx->slot_id, fsm->hdr.payload_len);
            drain_fd(ctx->fd_data);
            fsm->state = IPC_READ_IDLE;
            return;
        }
        /* Allocate buffer for payload (includes Footer) */
        fsm->buf = malloc(fsm->hdr.payload_len);
        if (!fsm->buf) {
            log_error("[IPC-%d] data malloc(%u) failed, marking worker dead",
                      ctx->slot_id, fsm->hdr.payload_len);
            worker_mark_dead(ctx, true);
            return;
        }
        fsm->state = IPC_READ_PAYLOAD;
        fsm->nread = 0;
    }

    /* --- PAYLOAD phase --- */
    if (fsm->state == IPC_READ_PAYLOAD) {
        rc = safe_ipc_recv_payload_fsm(ctx->fd_data, fsm);
        if (rc == -2) return;
        if (rc != 0) {
            log_error("[IPC-%d] data payload recv failed, marking worker dead", ctx->slot_id);
            free(fsm->buf); fsm->buf = NULL;
            worker_mark_dead(ctx, true);
            return;
        }
        fsm->state = IPC_READ_FOOTER;
        fsm->nread = 0;
    }

    /* --- FOOTER phase --- */
    if (fsm->state == IPC_READ_FOOTER) {
        uint64_t footer_magic = 0;
        rc = safe_ipc_recv_footer_fsm(ctx->fd_data, fsm, &footer_magic);
        if (rc == -2) return;
        if (rc != 0 || footer_magic != IPC_FOOTER_MAGIC) {
            log_error("[IPC-%d] data footer mismatch (got=0x%016llx expected=0x%016llx), dropping batch",
                      ctx->slot_id, (unsigned long long)footer_magic,
                      (unsigned long long)IPC_FOOTER_MAGIC);
            free(fsm->buf); fsm->buf = NULL;
            fsm->state = IPC_READ_IDLE;
            return;
        }
    }

    /* --- Complete: strip Footer and forward to Master --- */
    {
        void *payload = fsm->buf;
        uint32_t net_payload_len = fsm->hdr.payload_len - sizeof(uint64_t);
        /* Reset FSM */
        fsm->state = IPC_READ_IDLE;
        fsm->nread = 0;
        fsm->buf = NULL;

        log_debug_v(202605150000, "[IPC-%d] received BATCH (net_payload=%u), forwarding RET_BATCH",
                    ctx->slot_id, net_payload_len);
        send_return(ctx, RET_BATCH, payload, net_payload_len);
        /* ownership transferred */
    }
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

            /* v15.4.0: reset FSM states for new Worker */
            ctx->ctrl_fsm.state = IPC_READ_IDLE;
            ctx->ctrl_fsm.nread = 0;
            free(ctx->ctrl_fsm.buf); ctx->ctrl_fsm.buf = NULL;
            ctx->data_fsm.state = IPC_READ_IDLE;
            ctx->data_fsm.nread = 0;
            free(ctx->data_fsm.buf); ctx->data_fsm.buf = NULL;

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

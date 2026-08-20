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
#include <fcntl.h>
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
                        send_return(ctx, RET_HEARTBEAT, ret, sizeof(*ret), 0);
                    }
                }
                free(payload);
                break;
            }
            case IPC_MSG_ERROR:
            case IPC_MSG_ENTRY_ERROR: {
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
                        send_return(ctx,
                                    hdr.msg_type == IPC_MSG_ENTRY_ERROR ? RET_ENTRY_ERROR : RET_ERROR,
                                    ret, sizeof(*ret), 0);
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
                        send_return(ctx, RET_DEV_TIMEOUT, ret, sizeof(*ret), 0);
                    }
                }
                free(payload);
                break;
            }
            case IPC_MSG_READY: {
                log_debug_v(202605181600UL, "[IPC-%d] received READY, forwarding RET_READY", ctx->slot_id);
                send_return(ctx, RET_READY, NULL, 0, 0);
                free(payload);
                break;
            }
            case IPC_MSG_FINISH: {
                if (hdr.payload_len >= sizeof(IpcFinishPayload)) {
                    IpcFinishPayload *fin = (IpcFinishPayload*)payload;
                    log_info("[IPC-%d] received FINISH (path_len=%u), forwarding RET_FINISH", ctx->slot_id, fin->path_len);
                    /* v15.6.0: 转发 FINISH 前先排空 fd_data 中该任务的全部 BATCH。
                     * fd_data/fd_ctrl 是独立通道，Master 若先消费 FINISH 会把 Worker
                     * 置 IDLE 并派发新任务（epoch 递增），滞留的旧 BATCH 会被 epoch
                     * 校验误杀（文件+子目录丢失）。Worker 协议保证 BATCH 全部写完
                     * 才写 FINISH，故 FINISH 可读时所有 BATCH 字节已在 fd_data
                     * 内核缓冲，排至 EAGAIN 即完整。Phase 2 的 ALL_BATCHES_RECEIVED
                     * 屏障将在此基础上提供完整的目录级闭环。 */
                    for (int guard = 0; guard < 100000 && ctx->fd_data >= 0; guard++) {
                        struct pollfd pfd = { .fd = ctx->fd_data, .events = POLLIN };
                        if (poll(&pfd, 1, 0) <= 0 || !(pfd.revents & POLLIN)) break;
                        read_data_message(ctx);
                    }
                    size_t path_len = fin->path_len;
                    if (path_len > 4095) path_len = 4095;
                    char *path_buf = malloc(path_len + 1);
                    if (path_buf) {
                        if (hdr.payload_len >= sizeof(IpcFinishPayload) + path_len) {
                            memcpy(path_buf, (char*)payload + sizeof(IpcFinishPayload), path_len);
                        }
                        path_buf[path_len] = '\0';
                        /* v15.6.0: 回带 pipe 中解析出的 epoch */
                        send_return(ctx, RET_FINISH, path_buf, path_len + 1, fin->epoch);
                    }
                }
                free(payload);
                break;
            }
            case IPC_MSG_EXIT: {
                send_return(ctx, RET_EXIT, NULL, 0, 0);
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
        uint32_t payload_body_len = fsm->hdr.payload_len - sizeof(uint64_t);
        rc = fsm_recv(ctx->fd_data, fsm->buf, payload_body_len, &fsm->nread);
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
        uint8_t footer_buf[sizeof(uint64_t)];
        rc = fsm_recv(ctx->fd_data, footer_buf, sizeof(footer_buf), &fsm->nread);
        if (rc == -2) return;
        if (rc != 0) {
            log_error("[IPC-%d] data footer recv failed, dropping batch", ctx->slot_id);
            free(fsm->buf); fsm->buf = NULL;
            fsm->state = IPC_READ_IDLE;
            return;
        }
        uint64_t footer_magic = 0;
        memcpy(&footer_magic, footer_buf, sizeof(footer_magic));
        if (footer_magic != IPC_FOOTER_MAGIC) {
            log_error("[IPC-%d] data footer mismatch (got=0x%016llx expected=0x%016llx), dropping batch",
                      ctx->slot_id, (unsigned long long)footer_magic,
                      (unsigned long long)IPC_FOOTER_MAGIC);
            free(fsm->buf); fsm->buf = NULL;
            fsm->state = IPC_READ_IDLE;
            return;
        }
        /* Copy footer into buf so it matches Worker-side format */
        memcpy((uint8_t*)fsm->buf + fsm->hdr.payload_len - sizeof(uint64_t), footer_buf, sizeof(footer_buf));
    }

    /* --- Complete: strip Footer and forward to Master --- */
    {
        void *payload = fsm->buf;
        uint32_t net_payload_len = fsm->hdr.payload_len - sizeof(uint64_t);
        /* Reset FSM */
        fsm->state = IPC_READ_IDLE;
        fsm->nread = 0;
        fsm->buf = NULL;

        log_debug_v(202605181600UL, "[IPC-%d] received BATCH (net_payload=%u), forwarding RET_BATCH",
                    ctx->slot_id, net_payload_len);
        /* v15.6.0: 从 BATCH payload 头解析 epoch，随 RET_BATCH 携带给 Master 校验 */
        uint64_t epoch = 0;
        if (net_payload_len >= sizeof(IpcBatchHeader)) {
            IpcBatchHeader bh;
            memcpy(&bh, payload, sizeof(bh));
            epoch = bh.epoch;
        }
        send_return(ctx, RET_BATCH, payload, net_payload_len, epoch);
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
                /* v15.6.0: Replacement 窗口期（fd_cmd 尚未就绪）——暂存该 CMD_SCAN
                 * （每 IPC 线程一条 pending slot），待 CMD_REPLACE 完成后补发；
                 * 若 CMD_REPLACE 长时间未到，由心跳超时路径自然处理。
                 * Master 按 IDLE 状态派发，同 slot 同时只有一条在途 SCAN，
                 * pending 已被占用属设计外异常，log_fatal 暴露。 */
                if (ctx->has_pending_scan) {
                    log_fatal("[IPC-%d] pending CMD_SCAN slot occupied, overwriting (old=%s)",
                              ctx->slot_id, ((CmdScanPayload*)ctx->pending_scan.data)->path);
                    free(ctx->pending_scan.data);
                }
                ctx->pending_scan = *cmd;
                ctx->has_pending_scan = true;
                cmd->data = NULL; /* 所有权转移至 pending_scan */
                log_info_v(202608202330UL, "[IPC-%d] CMD_SCAN stashed during replacement window (path=%s)",
                           ctx->slot_id, path_log_mask(scan->path));
                break;
            }
            /* v15.6.0: SCAN wire payload = IpcScanHeader(epoch) + path */
            uint32_t scan_total = (uint32_t)sizeof(IpcScanHeader) + scan->path_len;
            uint8_t *scan_buf = malloc(scan_total);
            if (!scan_buf) {
                log_fatal("[IPC-%d] CMD_SCAN malloc(%u) failed", ctx->slot_id, scan_total);
                break;
            }
            IpcScanHeader sh = { scan->epoch };
            memcpy(scan_buf, &sh, sizeof(sh));
            memcpy(scan_buf + sizeof(sh), scan->path, scan->path_len);
            int rc = ipc_send(ctx->fd_cmd, IPC_MSG_SCAN, scan_buf, scan_total);
            free(scan_buf);
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
                /* v15.6.0: 容量 65536，重推失败属设计外异常——SCAN 丢失会导致
                 * pending_tasks 泄漏，log_fatal 暴露，不得静默丢弃 */
                if (!msg_queue_send(ctx->cmd_queue, cmd)) {
                    log_fatal("[IPC-%d] CMD_SCAN EAGAIN, cmd_queue full, cannot requeue %s",
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
                log_info_v(202605181600UL, "[IPC-%d] CMD_SCAN sent to worker (path=%s, len=%u)", ctx->slot_id, path_log_mask(scan->path), scan->path_len);
            }
            break;
        }
        case CMD_REPLACE: {
            CmdReplacePayload *rep = (CmdReplacePayload*)cmd->data;
            if (!rep) break;

            /* Close old fds — v15.6.0: close 前先把旧 fd 设为非阻塞并 read 到 EAGAIN，
             * drain 掉旧 Worker 残留在 pipe 中的数据，避免污染新 Worker 通道 */
            if (ctx->fd_cmd >= 0) { close(ctx->fd_cmd); ctx->fd_cmd = -1; }
            if (ctx->fd_data >= 0) {
                if (ctx->epfd >= 0) epoll_ctl(ctx->epfd, EPOLL_CTL_DEL, ctx->fd_data, NULL);
                int fl = fcntl(ctx->fd_data, F_GETFL);
                if (fl >= 0) fcntl(ctx->fd_data, F_SETFL, fl | O_NONBLOCK);
                drain_fd(ctx->fd_data);
                close(ctx->fd_data);
                ctx->fd_data = -1;
            }
            if (ctx->fd_ctrl >= 0) {
                if (ctx->epfd >= 0) epoll_ctl(ctx->epfd, EPOLL_CTL_DEL, ctx->fd_ctrl, NULL);
                int fl = fcntl(ctx->fd_ctrl, F_GETFL);
                if (fl >= 0) fcntl(ctx->fd_ctrl, F_SETFL, fl | O_NONBLOCK);
                drain_fd(ctx->fd_ctrl);
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

            /* v15.6.0: 补发替换窗口期暂存的 CMD_SCAN */
            if (ctx->has_pending_scan) {
                IpcThreadMsg pending = ctx->pending_scan;
                ctx->pending_scan.data = NULL;
                ctx->has_pending_scan = false;
                log_info("[IPC-%d] resending stashed CMD_SCAN after REPLACE", ctx->slot_id);
                handle_cmd(ctx, &pending);
            }
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

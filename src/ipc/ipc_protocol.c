/**
 * @file ipc_protocol.c
 * @brief IPC 协议封装：TLV 消息的发送、接收与管道排空
 *
 * 提供与 Worker 进程之间双向管道的底层通信原语：
 * - ipc_send / ipc_recv_header / ipc_recv_payload：原子化 TLV 消息读写
 * - ipc_drain_and_count_tasks：管道排空并统计遗留 SCAN 任务数
 */
#define _GNU_SOURCE
#include "ipc_protocol.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <poll.h>

/**
 * @brief  通过文件描述符发送 IPC 消息
 * @param  fd           int         目标文件描述符，取值范围: >= 0 的可写 fd
 * @param  msg_type     uint32_t    消息类型，取值范围: IPC_MSG_SCAN(1) ~ IPC_MSG_STOP(6)
 * @param  payload      const void* 消息负载数据指针，允许为 NULL（当 payload_len == 0 时）
 * @param  payload_len  uint32_t    负载数据长度（字节），取值范围: >= 0
 * @return int  返回 0 表示发送成功；返回 -1 表示发生致命错误（如管道破裂）；
 *              返回 -2 表示遇到 EAGAIN/EWOULDBLOCK（非阻塞模式下管道已满）
 *
 * @note   先发送固定 8 字节的 IpcMessageHeader，再发送变长 payload。
 *         对 EINTR 自动重试。Master 向 Worker 写 fd_in 时采用非阻塞模式，
 *         遇到 -2 时应将任务缓存到 WorkerSlot::backlog_paths。
 */
int ipc_send(int fd, uint32_t msg_type, const void *payload, uint32_t payload_len) {
    size_t total_len = sizeof(IpcMessageHeader) + payload_len;
    if (total_len > 4096 && msg_type != IPC_MSG_BATCH) {
        log_fatal("[ipc_send] total_len=%zu exceeds PIPE_BUF(4096), msg_type=%u payload_len=%u. "
                  "Ensure MAX_PATH_LENGTH <= 4088 to guarantee atomic pipe writes.",
                  total_len, msg_type, payload_len);
        return -1;
    }

    char *buf = malloc(total_len);
    if (!buf) return -1;

    IpcMessageHeader *hdr = (IpcMessageHeader*)buf;
    hdr->msg_type = msg_type;
    hdr->payload_len = payload_len;
    if (payload_len > 0 && payload) {
        memcpy(buf + sizeof(IpcMessageHeader), payload, payload_len);
    }

    size_t written = 0;
    int partial_retry = 0;  /* v15.4.1: limit partial-write retries to prevent IPC thread hang */
    log_debug_v(202605181600UL, "[ipc_send] fd=%d total=%zu msg_type=%u", fd, total_len, msg_type);
    while (written < total_len) {
        ssize_t n = write(fd, buf + written, total_len - written);
        if (n == 0) {
            log_error("[ipc_send] write returned 0 on fd=%d (EOF/unexpected), aborting", fd);
            free(buf);
            return -1;
        }
        if (n < 0) {
            int saved_errno = errno;
            log_debug_v(202605181600UL, "[ipc_send] write error fd=%d written=%zu n=%zd errno=%d (%s)",
                      fd, written, n, saved_errno, strerror(saved_errno));
            if (saved_errno == EINTR) continue;
            if (saved_errno == EAGAIN || saved_errno == EWOULDBLOCK) {
                if (written == 0) { free(buf); return -2; }
                /* Partial write occurred on non-blocking fd; retry with limit */
                partial_retry++;
                if (partial_retry > 1000) {
                    log_error("[ipc_send] partial write retry exhausted (1000x1ms) on fd=%d, aborting", fd);
                    free(buf);
                    return -1;
                }
                usleep(1000); /* 1ms back-off */
                continue;
            }
            free(buf);
            return -1;
        }
        written += n;
        log_debug_v(202605181600UL, "[ipc_send] write ok fd=%d written=%zu n=%zd", fd, written, n);
    }
    log_debug_v(202605181600UL, "[ipc_send] fd=%d total=%zu complete", fd, total_len);
    free(buf);
    return 0;
}

/**
 * @brief  从文件描述符接收 IPC 消息头部
 * @param  fd   int                 源文件描述符，取值范围: >= 0 的可读 fd
 * @param  hdr  IpcMessageHeader*   输出缓冲区指针，用于存放接收到的头部，不能为空
 * @return int  返回 0 表示接收成功；返回 -1 表示发生错误或遇到 EOF；
 *              返回 -2 表示遇到 EAGAIN/EWOULDBLOCK（非阻塞模式）
 *
 * @note   阻塞读取直到 8 字节头部完整接收。对 EINTR 自动重试。
 *         返回 -1 通常表示 Worker 进程已退出或管道被关闭。
 */
int ipc_recv_header(int fd, IpcMessageHeader *hdr) {
    size_t nread = 0;
    while (nread < sizeof(*hdr)) {
        ssize_t n = read(fd, (char*)hdr + nread, sizeof(*hdr) - nread);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return -2;
            int saved_errno = errno;
            log_error("[IPC] recv_header error on fd=%d: errno=%d (%s)",
                    fd, saved_errno, strerror(saved_errno));
            return -1;
        }
        if (n == 0) {
            log_error("[IPC] recv_header EOF on fd=%d", fd);
            return -1; /* EOF */
        }
        nread += n;
    }
    return 0;
}

/**
 * @brief  从文件描述符接收 IPC 消息负载
 * @param  fd   int    源文件描述符，取值范围: >= 0 的可读 fd
 * @param  buf  void*  负载接收缓冲区指针，不能为空
 * @param  len  uint32_t  要接收的字节数，取值范围: >= 0
 * @return int  返回 0 表示接收成功；返回 -1 表示发生错误或遇到 EOF；
 *              返回 -2 表示遇到 EAGAIN/EWOULDBLOCK（非阻塞模式）
 *
 * @note   当 len == 0 时立即返回 0。对 EINTR 自动重试。
 *         本函数应在 ipc_recv_header 确认 payload_len 后调用。
 */
int ipc_recv_payload(int fd, void *buf, uint32_t len) {
    if (len == 0) return 0;
    size_t nread = 0;
    while (nread < len) {
        ssize_t n = read(fd, (char*)buf + nread, len - nread);
        if (n < 0) { if (errno == EINTR) continue; if (errno == EAGAIN || errno == EWOULDBLOCK) return -2; return -1; }
        if (n == 0) return -1;
        nread += n;
    }
    return 0;
}

/**
 * @brief  排空 worker 的 fd_in 管道并统计其中未处理的 SCAN 任务数量
 * @param  fd_in  int  worker 的输入管道 fd（master 写入端已关闭，此处读取剩余端）
 * @return int  返回排空的 SCAN 任务数量
 *
 * @note   在 monitor kill worker 后调用，用于精确递减 pending_tasks，防止任务幽灵化。
 *         对 EAGAIN 静默处理（非阻塞管道正常结束条件），不打印错误日志。
 */
int ipc_drain_and_count_tasks(int fd_in) {
    if (fd_in < 0) return 0;
    int count = 0;
    while (1) {
        IpcMessageHeader hdr;
        ssize_t n = read(fd_in, (char*)&hdr, sizeof(hdr));
        if (n < 0) {
            if (errno == EINTR) continue;
            break; /* EAGAIN or real error -> pipe empty */
        }
        if (n == 0) break; /* EOF */
        if ((size_t)n < sizeof(hdr)) break; /* partial header -> stop */

        if (hdr.msg_type == IPC_MSG_SCAN) count++;

        if (hdr.payload_len > 0) {
            uint8_t *buf = malloc(hdr.payload_len);
            if (buf) {
                size_t nread = 0;
                while (nread < hdr.payload_len) {
                    ssize_t nr = read(fd_in, buf + nread, hdr.payload_len - nread);
                    if (nr < 0) {
                        if (errno == EINTR) continue;
                        break;
                    }
                    if (nr == 0) break;
                    nread += nr;
                }
                free(buf);
            }
        }
    }
    return count;
}

/* ================================================================
 * v15.4.0: FSM-based resumable IPC receive
 * Cross-epoll stateful reads with poll timeout & EAGAIN resumption
 * ================================================================ */

/**
 * @brief  通用续传 read 函数
 * @param  fd     int      文件描述符
 * @param  dst    void*    目标缓冲区
 * @param  total  size_t   需要读取的总字节数
 * @param  nread  size_t*  已读字节数（输入输出参数，跨调用保持）
 * @return int  返回 0 表示完成；返回 -2 表示 EAGAIN/EWOULDBLOCK（需下次 epoll 续传）；
 *              返回 -1 表示错误或 EOF
 *
 * @note   本函数不重置 nread；调用方在进入新阶段前自行置 0。
 *         对 EINTR 自动重试。poll 超时 100ms。
 */
int fsm_recv(int fd, void *dst, size_t total, size_t *nread) {
    while (*nread < total) {
        struct pollfd pfd = { fd, POLLIN, 0 };
        int rc = poll(&pfd, 1, 100);
        if (rc == 0) return -2; /* timeout */
        if (rc < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) return -1;

        ssize_t n = read(fd, (char*)dst + *nread, total - *nread);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return -2;
            return -1;
        }
        if (n == 0) return -1; /* EOF */
        *nread += (size_t)n;
    }
    return 0;
}

/**
 * @brief  通过 FSM 安全读取 IPC 消息头部
 * @param  fd   int         文件描述符
 * @param  fsm  IpcReadFsm* 读取状态机
 * @return int  返回 0 表示头部读取完成；返回 -2 表示需下次 epoll 续传；
 *              返回 -1 表示错误或 EOF
 *
 * @note   首次调用时 fsm->state 应为 IPC_READ_HDR，fsm->nread 应为 0。
 *         头部读取完成后 fsm->state 仍为 IPC_READ_HDR，调用方应自行转换。
 */
int safe_ipc_recv_header_fsm(int fd, IpcReadFsm *fsm) {
    if (fsm->state != IPC_READ_HDR) return -1;
    int rc = fsm_recv(fd, &fsm->hdr, sizeof(fsm->hdr), &fsm->nread);
    if (rc == 0) {
        /* Header complete — caller transitions state */
    }
    return rc;
}

/**
 * @brief  通过 FSM 安全读取 IPC 消息负载
 * @param  fd   int         文件描述符
 * @param  fsm  IpcReadFsm* 读取状态机
 * @return int  返回 0 表示负载读取完成；返回 -2 表示需下次 epoll 续传；
 *              返回 -1 表示错误或 EOF；返回 -3 表示内存分配失败
 *
 * @note   首次进入本状态前，调用方应分配 fsm->buf = malloc(len) 并置 fsm->nread = 0。
 */
int safe_ipc_recv_payload_fsm(int fd, IpcReadFsm *fsm) {
    if (fsm->state != IPC_READ_PAYLOAD) return -1;
    if (!fsm->buf) return -3;
    return fsm_recv(fd, fsm->buf, fsm->hdr.payload_len, &fsm->nread);
}

/**
 * @brief  通过 FSM 读取 Footer 魔数并校验
 * @param  fd        int         文件描述符
 * @param  fsm       IpcReadFsm* 读取状态机
 * @param  out_magic uint64_t*   输出读取到的魔数值
 * @return int  返回 0 表示 Footer 读取完成且已校验（out_magic 有效）；
 *              返回 -2 表示需下次 epoll 续传；返回 -1 表示错误或 EOF
 *
 * @note   首次进入本状态前，调用方应置 fsm->nread = 0。
 *         成功读取后，调用方需自行比较 *out_magic == IPC_FOOTER_MAGIC。
 */
int safe_ipc_recv_footer_fsm(int fd, IpcReadFsm *fsm, uint64_t *out_magic) {
    if (fsm->state != IPC_READ_FOOTER) return -1;
    uint8_t buf[sizeof(uint64_t)];
    int rc = fsm_recv(fd, buf, sizeof(buf), &fsm->nread);
    if (rc == 0 && out_magic) {
        memcpy(out_magic, buf, sizeof(*out_magic));
    }
    return rc;
}

/**
 * @brief  校验 IPC 消息类型是否在白名单内
 * @param  msg_type  uint32_t  消息类型值
 * @return bool  true 表示合法类型；false 表示垃圾/未知类型
 *
 * @note   v15.4.0 防御性加固：Header 被污染时快速拒绝，避免无意义 malloc。
 */
bool ipc_msg_type_valid(uint32_t msg_type) {
    switch (msg_type) {
        case IPC_MSG_SCAN:
        case IPC_MSG_BATCH:
        case IPC_MSG_HEARTBEAT:
        case IPC_MSG_ERROR:
        case IPC_MSG_EXIT:
        case IPC_MSG_STOP:
        case IPC_MSG_DEV_TIMEOUT:
        case IPC_MSG_READY:
        case IPC_MSG_FINISH:
            return true;
        default:
            return false;
    }
}


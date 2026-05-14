/**
 * @file worker_proc.c
 * @brief Worker 子进程实现、IPC 协议封装与进程池管理
 *
 * Worker 进程通过 fork() 创建，与 Master 之间通过双向 pipe 进行 TLV 格式的 IPC 通信。
 * Worker 负责阻塞读取 fd_in 上的扫描任务，执行 readdir + lstat（或 blind-trust 跳过），
 * 将结果批次通过 fd_out 写回 Master。
 * Master 侧提供进程池管理：创建、销毁、替换 Worker，以及处理管道积压（backlog）。
 */
#define _GNU_SOURCE
#include "worker_proc.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>
#include <sys/wait.h>

/* Read-only context inherited via fork (COW, never modified by parent after fork) */
static const Config *g_worker_cfg = NULL;
static const FingerprintSet *g_worker_ref_set = NULL;
static const ReferenceMap *g_worker_ref_map = NULL;

/**
 * @brief  设置 Worker 进程只读上下文（fork 前由主进程调用）
 * @param  cfg      const Config*        全局配置指针，允许为 NULL
 * @param  ref_set  const FingerprintSet* 半增量参考指纹集合指针，允许为 NULL（非半增量模式）
 * @param  ref_map  const ReferenceMap*   半增量参考映射表指针，允许为 NULL（非半增量模式）
 * @return void
 *
 * @note   这些指针仅在 Worker 进程（fork 后的子进程）中只读访问。
 *         利用 Linux 的写时复制（COW）机制，实现零拷贝共享上下文。
 */
void worker_set_context(const Config *cfg, const FingerprintSet *ref_set, const ReferenceMap *ref_map) {
    g_worker_cfg = cfg;
    g_worker_ref_set = ref_set;
    g_worker_ref_map = ref_map;
}

/* ================================================================
 * IPC helpers
 * ================================================================ */

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
    IpcMessageHeader hdr = { msg_type, payload_len };
    size_t written = 0;
    while (written < sizeof(hdr)) {
        ssize_t n = write(fd, (const char*)&hdr + written, sizeof(hdr) - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return -2;
            return -1;
        }
        written += n;
    }
    if (payload_len > 0 && payload) {
        written = 0;
        while (written < payload_len) {
            ssize_t n = write(fd, (const char*)payload + written, payload_len - written);
            if (n < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) return -2;
                return -1;
            }
            written += n;
        }
    }
    return 0;
}

/**
 * @brief  从文件描述符接收 IPC 消息头部
 * @param  fd   int                 源文件描述符，取值范围: >= 0 的可读 fd
 * @param  hdr  IpcMessageHeader*   输出缓冲区指针，用于存放接收到的头部，不能为空
 * @return int  返回 0 表示接收成功；返回 -1 表示发生错误或遇到 EOF
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
            fprintf(stderr, "[IPC] recv_header error on fd=%d: errno=%d (%s)\n",
                    fd, saved_errno, strerror(saved_errno));
            return -1;
        }
        if (n == 0) {
            fprintf(stderr, "[IPC] recv_header EOF on fd=%d\n", fd);
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
 * @return int  返回 0 表示接收成功；返回 -1 表示发生错误或遇到 EOF
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
 * Worker-side scan logic
 * ================================================================ */

/**
 * @brief  将 dirent::d_type 转换为 stat::st_mode 中的文件类型位
 * @param  d_type  unsigned char  dirent 中的 d_type 值，取值范围: DT_REG/DT_DIR/DT_LNK/DT_CHR/DT_BLK/DT_FIFO/DT_SOCK/DT_UNKNOWN
 * @return mode_t  对应的 S_IF* 文件类型位；若 d_type 未知则返回 0
 *
 * @note   用于 blind-trust 场景下，当 Worker 跳过 lstat 时，
 *         根据 dirent 中的 d_type 构造一个最小可用的 stat 结构体。
 */
static mode_t dt_to_mode(unsigned char d_type) {
    switch (d_type) {
        case DT_REG:  return S_IFREG;
        case DT_DIR:  return S_IFDIR;
        case DT_LNK:  return S_IFLNK;
        case DT_CHR:  return S_IFCHR;
        case DT_BLK:  return S_IFBLK;
        case DT_FIFO: return S_IFIFO;
        case DT_SOCK: return S_IFSOCK;
        default:      return 0;
    }
}

/**
 * @brief  尝试对已知文件执行 blind-trust（跳过 lstat）
 * @param  full_path  const char*      文件绝对路径，不能为空
 * @param  dir_dev    uint64_t         父目录所在设备号
 * @param  d_ino      uint64_t         文件的 inode 号（来自 dirent）
 * @param  d_type     unsigned char    文件类型（来自 dirent），取值范围: DT_REG/DT_DIR/...
 * @param  out_st     struct stat*     输出缓冲区，用于存放构造的 stat 信息，不能为空
 * @return bool  返回 true 表示 blind-trust 成功，out_st 已填充；false 表示无法信任，需要执行 lstat
 *
 * @note   信任条件：
 *         1. 半增量模式已启用（g_worker_ref_set 和 g_worker_ref_map 均不为 NULL）
 *         2. d_type 和 d_ino 均有效（非 DT_UNKNOWN、非 0）
 *         3. 指纹存在于 reference_set 中
 *         4. reference_map 中存在匹配记录且 d_type 一致
 *         5. 当前时间与 mtime 的差值超过 skip_interval
 *         满足以上条件时，直接用历史 mtime 构造 stat，避免 lstat 系统调用。
 */
static bool try_blind_trust(const char *full_path, uint64_t dir_dev, uint64_t d_ino,
                            unsigned char d_type, struct stat *out_st) {
    if (!g_worker_ref_set || !g_worker_ref_map) return false;
    if (d_type == DT_UNKNOWN || d_ino == 0) return false;

    uint8_t fp[FP_SIZE];
    fp_compute(full_path, dir_dev, d_ino, fp);

    if (!fp_set_contains(g_worker_ref_set, fp)) return false;

    const ReferenceEntry *ref = ref_map_lookup(g_worker_ref_map, fp);
    if (!ref || ref->d_type != d_type) return false;

    time_t now = time(NULL);
    if (g_worker_cfg->skip_interval <= 0) return false;
    if (now - ref->mtime <= g_worker_cfg->skip_interval) return false;

    memset(out_st, 0, sizeof(*out_st));
    out_st->st_dev   = dir_dev;
    out_st->st_ino   = d_ino;
    out_st->st_mtime = ref->mtime;
    out_st->st_mode  = dt_to_mode(d_type);
    return true;
}

/**
 * @brief  向 Master 发送一批扫描结果
 * @param  fd_out  int            输出文件描述符（指向 Master 的 fd_out），取值范围: >= 0 的可写 fd
 * @param  paths   char**         文件路径字符串数组，允许为 NULL（当 count == 0 时）
 * @param  stats   struct stat*   对应的 stat 信息数组，允许为 NULL（当 count == 0 时）
 * @param  count   int            本次批次中的文件数量，取值范围: >= 0
 * @return void
 *
 * @note   即使 count == 0 也会发送空批次，确保 Master 的 pending_tasks 正确递减。
 *         负载格式：IpcBatchHeader + count * ([uint32_t plen][char path[plen]][struct stat st])。
 *         Worker 侧遇到 EAGAIN 时以 1ms 间隔重试，直至成功。
 *         若内存分配失败，递归发送空批次防止 Master 挂起。
 */
static void send_batch(int fd_out, char **paths, struct stat *stats, int count) {
    /* Always send a batch (even count==0) so Master can decrement pending_tasks */

    /* Calculate total payload size */
    size_t total = sizeof(IpcBatchHeader);
    for (int i = 0; i < count; i++) {
        total += sizeof(uint32_t);
        total += strlen(paths[i]);
        total += sizeof(struct stat);
    }

    if (total > UINT32_MAX) {
        fprintf(stderr, "[Worker] Batch payload too large (%zu), aborting.\n", total);
        return;
    }

    uint8_t *buf = malloc(total);
    if (!buf) {
        /* 内存不足时发送空 batch，确保 Master 能正确递减 pending_tasks */
        send_batch(fd_out, NULL, NULL, 0);
        return;
    }

    uint8_t *p = buf;
    IpcBatchHeader bh = { (uint32_t)count };
    memcpy(p, &bh, sizeof(bh)); p += sizeof(bh);

    for (int i = 0; i < count; i++) {
        uint32_t plen = (uint32_t)strlen(paths[i]);
        memcpy(p, &plen, sizeof(plen)); p += sizeof(plen);
        memcpy(p, paths[i], plen);      p += plen;
        memcpy(p, &stats[i], sizeof(struct stat)); p += sizeof(struct stat);
    }

    /* Worker side: retry on EAGAIN until success (pipe buffer should be large enough) */
    int rc;
    while ((rc = ipc_send(fd_out, IPC_MSG_BATCH, buf, (uint32_t)total)) == -2) {
        usleep(1000); /* 1ms */
    }
    free(buf);
}

/**
 * @brief  发送设备级错误通知并追加空批次
 * @param  fd_out    int          输出文件描述符，取值范围: >= 0 的可写 fd
 * @param  err_code  int          错误码，取值范围: ETIMEDOUT(110)、EIO(5) 等系统 errno
 * @param  path      const char*  发生错误的文件/目录路径，不能为空
 * @return void
 *
 * @note   仅在 err_code 为 ETIMEDOUT 或 EIO 时发送 IPC_MSG_ERROR，
 *         其他错误码仅发送空批次。空批次确保 Master 正确递减 pending_tasks。
 */
static void send_error_and_empty_batch(int fd_out, int err_code, const char *path) {
    if (err_code == ETIMEDOUT || err_code == EIO) {
        IpcErrorHeader eh = { (uint32_t)err_code, 0 };
        uint32_t plen = (uint32_t)strlen(path);
        uint8_t *buf = malloc(sizeof(eh) + sizeof(plen) + plen);
        if (buf) {
            memcpy(buf, &eh, sizeof(eh));
            memcpy(buf + sizeof(eh), &plen, sizeof(plen));
            memcpy(buf + sizeof(eh) + sizeof(plen), path, plen);
            ipc_send(fd_out, IPC_MSG_ERROR, buf, (uint32_t)(sizeof(eh) + sizeof(plen) + plen));
            free(buf);
        }
    }
    send_batch(fd_out, NULL, NULL, 0);
}

/**
 * @brief  扫描单个目录并将结果批次发送回 Master
 * @param  fd_out     int          输出文件描述符，取值范围: >= 0 的可写 fd
 * @param  dir_path   const char*  要扫描的目录路径，不能为空
 * @param  worker_id  int          Worker 编号（当前未使用，保留用于日志），取值范围: >= 0
 * @return void
 *
 * @note   先对目录本身执行 lstat 获取设备号；然后 opendir/readdir 遍历条目。
 *         对每个条目：跳过 . 和 ..；尝试 blind-trust；失败则执行 lstat/stat；
 *         收集到 batch_size 条后发送批次；遍历结束后发送剩余批次（或空批次）。
 *         若 opendir 或 lstat 失败，发送错误通知和空批次。
 */
static void scan_and_send(int fd_out, const char *dir_path, int worker_id) {
    (void)worker_id;
    struct stat dir_st;
    if (lstat(dir_path, &dir_st) != 0) {
        send_error_and_empty_batch(fd_out, errno, dir_path);
        return;
    }

    uint64_t dir_dev = dir_st.st_dev;
    int batch_size = 1024;
    if (g_worker_cfg && g_worker_cfg->batch_size > 0)
        batch_size = g_worker_cfg->batch_size;

    char **paths = calloc(batch_size, sizeof(char*));
    struct stat *stats = calloc(batch_size, sizeof(struct stat));
    int count = 0;

    DIR *dir = opendir(dir_path);
    if (!dir) {
        send_error_and_empty_batch(fd_out, errno, dir_path);
        goto cleanup;
    }

    struct dirent *entry;
    int entries_since_hb = 0;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.' &&
            (entry->d_name[1] == '\0' || (entry->d_name[1] == '.' && entry->d_name[2] == '\0'))) {
            continue;
        }

        char full_path[4096];
        int n = snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);
        if (n >= (int)sizeof(full_path)) continue;

        struct stat st;
        bool got = false;

        if (try_blind_trust(full_path, dir_dev, entry->d_ino, entry->d_type, &st)) {
            got = true;
        } else {
            if (g_worker_cfg && g_worker_cfg->follow_symlinks) {
                if (stat(full_path, &st) != 0) continue;
            } else {
                if (lstat(full_path, &st) != 0) continue;
            }
            got = true;
        }

        if (got) {
            paths[count] = strdup(full_path);
            stats[count] = st;
            count++;
        }

        /* Send intermediate heartbeat every 256 entries to prevent monitor timeout on huge dirs */
        if (++entries_since_hb >= 256) {
            IpcHeartbeatPayload hb = { (uint64_t)time(NULL) };
            ipc_send(fd_out, IPC_MSG_HEARTBEAT, &hb, sizeof(hb));
            entries_since_hb = 0;
        }

        if (count >= batch_size) {
            send_batch(fd_out, paths, stats, count);
            for (int i = 0; i < count; i++) free(paths[i]);
            count = 0;
        }
    }

    if (count > 0) {
        send_batch(fd_out, paths, stats, count);
        for (int i = 0; i < count; i++) free(paths[i]);
    } else {
        /* Empty directory: send empty batch so Master decrements pending_tasks */
        send_batch(fd_out, NULL, NULL, 0);
    }

    closedir(dir);
cleanup:
    free(paths);
    free(stats);
}

/* ================================================================
 * Worker process entry
 * ================================================================ */

/**
 * @brief  Worker 子进程主入口函数
 * @param  fd_in      int  读取 Master 任务的输入管道 fd，取值范围: >= 0 的可读 fd
 * @param  fd_out     int  向 Master 发送结果的输出管道 fd，取值范围: >= 0 的可写 fd
 * @param  worker_id  int  Worker 编号（用于日志和调试），取值范围: >= 0
 * @return void
 *
 * @note   阻塞循环：接收 IPC 消息头部 → 根据 msg_type 处理：
 *         - IPC_MSG_SCAN：接收路径 → 发送扫描前心跳 → scan_and_send → 发送扫描后心跳
 *         - IPC_MSG_STOP：发送 IPC_MSG_EXIT 后退出循环
 *         - 其他类型：排空未知负载后继续循环
 *         遇到管道 EOF 或内存分配失败时直接 break 退出。
 */
void worker_main(int fd_in, int fd_out, int worker_id) {
    (void)worker_id;
    while (1) {
        IpcMessageHeader hdr;
        if (ipc_recv_header(fd_in, &hdr) != 0) {
            fprintf(stderr, "[Worker] worker_main exiting: ipc_recv_header failed on fd_in=%d\n", fd_in);
            break;
        }

        if (hdr.msg_type == IPC_MSG_STOP) {
            ipc_send(fd_out, IPC_MSG_EXIT, NULL, 0);
            break;
        }

        if (hdr.msg_type != IPC_MSG_SCAN) {
            /* drain unknown payload */
            if (hdr.payload_len > 0) {
                void *tmp = malloc(hdr.payload_len);
                ipc_recv_payload(fd_in, tmp, hdr.payload_len);
                free(tmp);
            }
            continue;
        }

        char *dir_path = malloc(hdr.payload_len + 1);
        if (!dir_path) break;
        if (ipc_recv_payload(fd_in, dir_path, hdr.payload_len) != 0) {
            free(dir_path);
            break;
        }
        dir_path[hdr.payload_len] = '\0';

        /* heartbeat before scan */
        IpcHeartbeatPayload hb = { (uint64_t)time(NULL) };
        ipc_send(fd_out, IPC_MSG_HEARTBEAT, &hb, sizeof(hb));

        scan_and_send(fd_out, dir_path, worker_id);

        /* heartbeat after scan */
        hb.timestamp = (uint64_t)time(NULL);
        ipc_send(fd_out, IPC_MSG_HEARTBEAT, &hb, sizeof(hb));

        free(dir_path);
    }
}

/* ================================================================
 * Master-side worker pool management
 * ================================================================ */

/**
 * @brief  创建 Worker 进程池
 * @param  num_workers  int  Worker 进程数量，取值范围: > 0
 * @return WorkerPool*  成功返回指向新分配进程池的指针；内存不足时返回 NULL
 *
 * @note   仅分配结构体内存和 slots 数组，不实际 fork 子进程。
 *         实际 spawn 需调用 worker_pool_spawn。
 */
WorkerPool* worker_pool_create(int num_workers) {
    WorkerPool *pool = calloc(1, sizeof(WorkerPool));
    if (!pool) return NULL;
    pool->slots = calloc(num_workers, sizeof(WorkerSlot));
    if (!pool->slots) { free(pool); return NULL; }
    pool->num_workers = num_workers;
    pool->active_count = 0;
    return pool;
}

/**
 * @brief  销毁 Worker 进程池并清理所有资源
 * @param  pool  WorkerPool*  要销毁的进程池指针，允许传入 NULL（空操作）
 * @return void
 *
 * @note   对存活的 Worker 发送 SIGKILL（不阻塞等待，避免 D-State 挂起），
 *         关闭所有管道 fd，释放 backlog_paths 中的路径内存。
 *         最后以非阻塞方式收割所有僵尸子进程（waitpid(-1, WNOHANG)）。
 */
void worker_pool_destroy(WorkerPool *pool) {
    if (!pool) return;
    for (int i = 0; i < pool->num_workers; i++) {
        WorkerSlot *slot = &pool->slots[i];
        if (slot->is_alive) {
            kill(slot->pid, SIGKILL);
            close(slot->fd_in);
            if (slot->fd_in_rd >= 0) close(slot->fd_in_rd);
            close(slot->fd_out);
        }
        /* Free backlog paths */
        for (int j = 0; j < slot->backlog_count; j++) {
            free(slot->backlog_paths[j]);
        }
        free(slot->backlog_paths);
    }
    /* Non-blocking reap of any zombie children */
    for (int i = 0; i < pool->num_workers * 3; i++) {
        if (waitpid(-1, NULL, WNOHANG) <= 0) break;
    }
    free(pool->slots);
    free(pool);
}

/**
 * @brief  扩大管道缓冲区容量（内部辅助函数）
 * @param  fd  int  要调整的管道文件描述符，取值范围: >= 0 的有效 fd
 * @return void
 *
 * @note   尝试将管道容量提升至 1MB（Linux 默认 64KB，上限由 /proc/sys/fs/pipe-max-size 决定，通常 1MB）。
 *         失败时静默忽略（fcntl 会返回错误但不影响功能）。
 */
static void enlarge_pipe(int fd) {
    int desired = 1024 * 1024; /* 1MB */
    int current = fcntl(fd, F_GETPIPE_SZ);
    if (current < desired) {
        fcntl(fd, F_SETPIPE_SZ, desired);
    }
}

/**
 * @brief  在指定 slot 中 fork 一个新的 Worker 子进程
 * @param  pool     WorkerPool*  目标进程池指针，不能为空
 * @param  slot_id  int          目标 slot 索引，取值范围: [0, pool->num_workers-1]
 * @return bool  返回 true 表示 fork 成功；false 表示失败（管道创建失败或 fork 失败）
 *
 * @note   创建双向 pipe2(O_CLOEXEC)，子进程关闭无关 fd 后进入 worker_main 循环。
 *         Master 的 fd_in 写端设置为非阻塞（O_NONBLOCK），配合 backlog 机制防止双向管道死锁。
 *         成功后会初始化 slot 的心跳时间和积压队列。
 */
bool worker_pool_spawn(WorkerPool *pool, int slot_id) {
    int in_pipe[2], out_pipe[2];
    if (pipe2(in_pipe, O_CLOEXEC) != 0) return false;
    /* O_NONBLOCK on out_pipe: prevents master read hang on stale fd reuse,
     * and prevents worker write hang when pipe buffer is full. */
    if (pipe2(out_pipe, O_CLOEXEC | O_NONBLOCK) != 0) {
        close(in_pipe[0]); close(in_pipe[1]);
        return false;
    }

    /* Enlarge pipe buffers to reduce deadlock probability */
    enlarge_pipe(in_pipe[0]); enlarge_pipe(in_pipe[1]);
    enlarge_pipe(out_pipe[0]); enlarge_pipe(out_pipe[1]);

    pid_t pid = fork();
    if (pid < 0) {
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        return false;
    }

    if (pid == 0) {
        /* Child */
        close(in_pipe[1]);
        close(out_pipe[0]);

        /* Close all inherited fds except our pipes */
        int max_fd = (int)sysconf(_SC_OPEN_MAX);
        if (max_fd < 0) max_fd = 65536;
        for (int fd = 3; fd < max_fd; fd++) {
            if (fd != in_pipe[0] && fd != out_pipe[1]) {
                close(fd);
            }
        }

        worker_main(in_pipe[0], out_pipe[1], slot_id);
        _exit(0);
    }

    /* Parent: keep in_pipe[0] for draining orphaned tasks after worker death */
    close(out_pipe[1]);

    /* Master write end must be non-blocking to prevent bidirectional pipe deadlock */
    int flags = fcntl(in_pipe[1], F_GETFL);
    if (flags >= 0) {
        fcntl(in_pipe[1], F_SETFL, flags | O_NONBLOCK);
    } else {
        fprintf(stderr, "[worker_pool_spawn] WARNING: fcntl(F_GETFL) on fd_in failed: errno=%d\n", errno);
    }

    /* out_pipe already has O_NONBLOCK from pipe2; no need for fragile fcntl here */

    WorkerSlot *slot = &pool->slots[slot_id];
    slot->pid = pid;
    slot->fd_in = in_pipe[1];
    slot->fd_in_rd = in_pipe[0];
    slot->fd_out = out_pipe[0];
    slot->is_alive = true;
    slot->last_heartbeat = time(NULL);
    slot->current_dev = 0;
    slot->current_path[0] = '\0';
    slot->backlog_paths = NULL;
    slot->backlog_count = 0;
    slot->backlog_capacity = 0;
    pool->active_count++;
    return true;
}

/**
 * @brief  替换指定 slot 中的 Worker 子进程（杀死旧进程并 spawn 新进程）
 * @param  pool     WorkerPool*  目标进程池指针，不能为空
 * @param  slot_id  int          目标 slot 索引，取值范围: [0, pool->num_workers-1]
 * @return bool  返回 true 表示替换成功；false 表示 spawn 新进程失败
 *
 * @note   对存活的旧 Worker 发送 SIGKILL 但不阻塞等待（waitpid WNOHANG），
 *         因为进程可能处于 D-State 不可杀死。旧进程成为僵尸后由主循环周期性收割。
 *         关闭旧 fd_in/fd_out，再调用 worker_pool_spawn 创建新进程。
 */
bool worker_pool_replace(WorkerPool *pool, int slot_id) {
    WorkerSlot *slot = &pool->slots[slot_id];
    if (slot->is_alive) {
        kill(slot->pid, SIGKILL);
        /* Do NOT block on waitpid: process may be stuck in D-state.
         * Zombie will be reaped later by main loop's periodic waitpid(-1, WNOHANG). */
        close(slot->fd_in);
        if (slot->fd_in_rd >= 0) {
            close(slot->fd_in_rd);
            slot->fd_in_rd = -1;
        }
        close(slot->fd_out);
        slot->is_alive = false;
        pool->active_count--;
    }
    return worker_pool_spawn(pool, slot_id);
}

/**
 * @brief  向所有存活的 Worker 发送停止指令（IPC_MSG_STOP）
 * @param  pool  WorkerPool*  目标进程池指针，允许传入 NULL（空操作）
 * @return void
 *
 * @note   采用 best-effort 策略：fd_in 可能为非阻塞且管道可能已满，
 *         发送失败不报错、不阻塞。Worker 收到 STOP 后发送 EXIT 并退出。
 *         若 Worker 因 D-State 无法响应 STOP，则由 monitor 通过 SIGKILL 强制替换。
 */
void worker_pool_stop_all(WorkerPool *pool) {
    for (int i = 0; i < pool->num_workers; i++) {
        if (pool->slots[i].is_alive) {
            int rc = ipc_send(pool->slots[i].fd_in, IPC_MSG_STOP, NULL, 0);
            (void)rc; /* STOP is best-effort; fd_in may be non-blocking */
        }
    }
}

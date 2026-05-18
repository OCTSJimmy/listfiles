/**
 * @file worker_scanner.c
 * @brief Worker 扫描引擎：目录遍历、blind-trust、批次发送与 Scanner 线程
 *
 * 包含 Worker 进程内部的扫描逻辑：
 * - scan_and_send：readdir + lstat（或 blind-trust 跳过）+ 批次发送
 * - worker_scanner_thread：Scanner 线程主循环，通过 pthread_cond 等待任务
 * - worker_set_context：fork 前由 Master 设置只读上下文（COW）
 */
#define _GNU_SOURCE
#include "worker_scanner.h"
#include "ipc_protocol.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>

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

/**
 * @brief  获取当前 Worker 配置指针
 * @return const Config*  当前配置指针；若未设置则返回 NULL
 *
 * @note   供 IPC 线程（worker_main）查询 heartbeat_timeout 等配置参数。
 */
const Config* worker_get_config(void) {
    return g_worker_cfg;
}

/* ================================================================
 * Scan helpers
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
    total += sizeof(uint64_t); /* v15.4.0: Footer magic */

    if (total > UINT32_MAX) {
        log_error("[Worker] Batch payload too large (%zu), aborting.", total);
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

    /* v15.4.0: append Footer magic */
    uint64_t footer = IPC_FOOTER_MAGIC;
    memcpy(p, &footer, sizeof(footer));

    /* Worker side: retry on EAGAIN until success (pipe buffer should be large enough) */
    int rc;
    while ((rc = ipc_send(fd_out, IPC_MSG_BATCH, buf, (uint32_t)total)) == -2) {
        usleep(1000); /* 1ms */
    }
    if (rc != 0) {
        log_error("[Worker] send_batch FAILED (rc=%d, total=%zu)", rc, total);
    } else {
        log_debug("[Worker] send_batch OK (total=%zu)", total);
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
    log_debug_v(202605181600, "[W%d-Scanner] scan_and_send entered: %s", worker_id, dir_path);
    struct stat dir_st;
    if (lstat(dir_path, &dir_st) != 0) {
        log_warn("[W%d-Scanner] lstat failed on %s: %s", worker_id, dir_path, strerror(errno));
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
        log_warn("[W%d-Scanner] opendir failed on %s: %s", worker_id, dir_path, strerror(errno));
        send_error_and_empty_batch(fd_out, errno, dir_path);
        goto cleanup;
    }
    log_debug_v(202605181600, "[W%d-Scanner] opendir success: %s", worker_id, dir_path);

    struct dirent *entry;
    int entry_count = 0;
    while ((entry = readdir(dir)) != NULL) {
        entry_count++;
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

        if (count >= batch_size) {
            send_batch(fd_out, paths, stats, count);
            for (int i = 0; i < count; i++) free(paths[i]);
            count = 0;
        }
    }

    if (count > 0) {
        log_debug_v(202605181600, "[W%d-Scanner] sending final batch (count=%d)", worker_id, count);
        send_batch(fd_out, paths, stats, count);
        for (int i = 0; i < count; i++) free(paths[i]);
    } else {
        /* Empty directory: send empty batch so Master decrements pending_tasks */
        log_debug_v(202605181600, "[W%d-Scanner] empty dir, sending empty batch", worker_id);
        send_batch(fd_out, NULL, NULL, 0);
    }

    log_debug_v(202605181600, "[W%d-Scanner] readdir loop done (entries=%d)", worker_id, entry_count);
    closedir(dir);
cleanup:
    free(paths);
    free(stats);
}

/* ================================================================
 * Scanner thread
 * ================================================================ */

void *worker_scanner_thread(void *arg) {
    WorkerThreadCtx *ctx = (WorkerThreadCtx *)arg;

    while (1) {
        pthread_mutex_lock(&ctx->task_mutex);
        while (!ctx->task_ready && !ctx->stop_flag) {
            pthread_cond_wait(&ctx->task_cond, &ctx->task_mutex);
        }
        if (ctx->stop_flag) {
            pthread_mutex_unlock(&ctx->task_mutex);
            break;
        }

        char path[4096];
        strncpy(path, ctx->task_path, sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';
        ctx->task_ready = false;
        pthread_mutex_unlock(&ctx->task_mutex);

        /* 记录扫描开始 */
        pthread_mutex_lock(&ctx->progress_mutex);
        ctx->last_progress = time(NULL);
        ctx->scanner_active = true;
        pthread_mutex_unlock(&ctx->progress_mutex);

        log_debug("[W%d-Scanner] start scanning: %s", ctx->worker_id, path);

        /* 扫描 — 结果通过 fd_data 发送 */
        scan_and_send(ctx->fd_data, path, ctx->worker_id);

        log_debug("[W%d-Scanner] scan_and_send returned: %s", ctx->worker_id, path);

        /* 发送 FINISH 信号，通知 Master 当前任务完成 */
        IpcFinishPayload fin = { 0, 0 };
        uint32_t plen = (uint32_t)strlen(path);
        fin.status = 0; /* OK */
        fin.path_len = plen;
        size_t fin_total = sizeof(fin) + plen;
        uint8_t *fin_buf = malloc(fin_total);
        if (fin_buf) {
            memcpy(fin_buf, &fin, sizeof(fin));
            memcpy(fin_buf + sizeof(fin), path, plen);
            int rc;
            int retry = 0;
            while ((rc = ipc_send(ctx->fd_ctrl, IPC_MSG_FINISH, fin_buf, (uint32_t)fin_total)) == -2) {
                usleep(1000);
                retry++;
                if (retry % 1000 == 0) {
                    log_warn("[W%d-Scanner] IPC_MSG_FINISH EAGAIN retry %d", ctx->worker_id, retry);
                }
            }
            log_debug("[W%d-Scanner] IPC_MSG_FINISH sent (rc=%d, path=%s, retries=%d)", ctx->worker_id, rc, path, retry);
            free(fin_buf);
        }

        /* 记录扫描完成 */
        pthread_mutex_lock(&ctx->progress_mutex);
        ctx->last_progress = time(NULL);
        ctx->scanner_active = false;
        pthread_mutex_unlock(&ctx->progress_mutex);
    }

    return NULL;
}

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
    if (d_type == DT_DIR) return false; /* v15.5.0: 目录始终不信任，避免 mtime 不可靠导致的遗漏 */
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

/* ================================================================
 * Scanner progress heartbeat (v15.5.3)
 * ================================================================ */

/* Forward declaration: scanner heartbeat callback into IPC thread */
extern void worker_scanner_progress(pthread_mutex_t *mutex, time_t *last_progress, int worker_id);

/**
 * @brief  定期更新 Scanner 进度时间戳，防止大目录 readdir 超时误判
 * @param  last_tick_time  time_t*  上次 tick 时间（输入输出）
 * @param  interval_sec    int      更新间隔（秒），默认 5
 * @param  worker_id       int      Worker 编号
 * @return void
 *
 * @note   v15.5.9: 从计数驱动（每 N 条目）改为时间驱动（每 interval_sec 秒）。
 *         NFS 大目录场景下，1000 个条目可能需要数分钟，计数驱动导致前 999 条无保护。
 *         时间驱动确保无论处理多慢，心跳持续更新。
 */
static void scanner_progress_tick(time_t *last_tick_time, int interval_sec,
                                  pthread_mutex_t *mutex, time_t *last_progress,
                                  int worker_id) {
    time_t now = time(NULL);
    if (now - *last_tick_time >= interval_sec) {
        pthread_mutex_lock(mutex);
        *last_progress = now;
        *last_tick_time = now;
        pthread_mutex_unlock(mutex);
        log_debug_v(202607030000UL, "[W%d-Scanner] progress tick (time-driven, interval=%ds)",
                    worker_id, interval_sec);
    }
}

/* ================================================================
 * Batch send helpers
 * ================================================================ */

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
 * @brief  发送目录级错误通知并追加空批次
 * @param  fd_data   int          数据通道 fd（空批次走这里），取值范围: >= 0 的可写 fd
 * @param  fd_ctrl   int          控制通道 fd（错误上报走这里），取值范围: >= 0 的可写 fd
 * @param  err_code  int          错误码，取值范围: ETIMEDOUT(110)、EIO(5)、EACCES(13) 等系统 errno
 * @param  dev       dev_t        当前任务所在设备号
 * @param  path      const char*  发生错误的文件/目录路径，不能为空
 * @return void
 *
 * @note   空批次确保 Master 正确递减 pending_tasks。
 *         v15.5.6: 填充真实 st_dev，修复之前硬编码 dev=0 的问题。
 *         v15.5.7: 错误上报从 fd_data 改到 fd_ctrl——此前误用 fd_data，Master 侧
 *         read_data_message 只接受 BATCH，非 BATCH 帧会被当作垃圾 drain 掉，
 *         导致 scanner 自检到的目录级错误永远到不了熔断清单；
 *         上报范围从仅 ETIMEDOUT/EIO 扩展到除 ENOENT/ENOTDIR 外的全部 errno
 *         （ENOENT/ENOTDIR 为扫描期间目录被并发删除/替换的正常竞态，不上报；
 *         EACCES 等此前静默丢失，会导致整棵子树缺失但扫描"成功"）。
 */
static void send_error_and_empty_batch(int fd_data, int fd_ctrl, int err_code, dev_t dev, const char *path) {
    if (err_code != ENOENT && err_code != ENOTDIR) {
        IpcErrorHeader eh = { (uint32_t)err_code, (uint64_t)dev };
        uint32_t plen = (uint32_t)strlen(path);
        uint8_t *buf = malloc(sizeof(eh) + sizeof(plen) + plen);
        if (buf) {
            memcpy(buf, &eh, sizeof(eh));
            memcpy(buf + sizeof(eh), &plen, sizeof(plen));
            memcpy(buf + sizeof(eh) + sizeof(plen), path, plen);
            int rc = ipc_send(fd_ctrl, IPC_MSG_ERROR, buf, (uint32_t)(sizeof(eh) + sizeof(plen) + plen));
            if (rc != 0)
                log_error("[Worker] send IPC_MSG_ERROR FAILED (rc=%d, path=%s)", rc, path);
            free(buf);
        }
    }
    send_batch(fd_data, NULL, NULL, 0);
}

/**
 * @brief  条目级错误上报（v15.5.7）
 * @param  fd_ctrl   int          控制通道 fd，取值范围: >= 0 的可写 fd
 * @param  err_code  int          错误码（lstat/stat 失败的 errno，或 ENAMETOOLONG 表示路径截断）
 * @param  dev       dev_t        条目所在设备号
 * @param  path      const char*  失败条目路径（截断时为父目录路径），不能为空
 * @return void
 *
 * @note   不排空批量、不触发设备惩罚/探测，仅通知 Master 将条目记入熔断清单
 *         并累加 skipped_count（扫描将以非零退出码结束）。
 */
static void send_entry_error(int fd_ctrl, int err_code, dev_t dev, const char *path) {
    if (fd_ctrl < 0) return;
    IpcErrorHeader eh = { (uint32_t)err_code, (uint64_t)dev };
    uint32_t plen = (uint32_t)strlen(path);
    uint8_t *buf = malloc(sizeof(eh) + sizeof(plen) + plen);
    if (!buf) return;
    memcpy(buf, &eh, sizeof(eh));
    memcpy(buf + sizeof(eh), &plen, sizeof(plen));
    memcpy(buf + sizeof(eh) + sizeof(plen), path, plen);
    int rc;
    while ((rc = ipc_send(fd_ctrl, IPC_MSG_ENTRY_ERROR, buf, (uint32_t)(sizeof(eh) + sizeof(plen) + plen))) == -2) {
        usleep(1000); /* 1ms */
    }
    if (rc != 0)
        log_error("[Worker] send_entry_error FAILED (rc=%d, path=%s)", rc, path);
    free(buf);
}

/**
 * @brief  条目级 stat（v15.5.7），带 EINTR 重试
 * @param  path  const char*  条目路径，不能为空
 * @param  st    struct stat* 输出缓冲区，不能为空
 * @return int  同 lstat/stat 返回值
 *
 * @note   信号（如 SIGALRM 探测计时器）可能中断慢速 NFS stat 返回 EINTR，
 *         若不重试会把信号中断误判为条目失败。最多重试 3 次。
 *         依配置 follow_symlinks 选择 stat 或 lstat。
 */
static int entry_stat(const char *path, struct stat *st) {
    int attempts = 0;
    int rc;
    do {
        rc = (g_worker_cfg && g_worker_cfg->follow_symlinks) ? stat(path, st) : lstat(path, st);
        attempts++;
    } while (rc != 0 && errno == EINTR && attempts < 3);
    return rc;
}

/**
 * @brief  扫描单个目录并将结果批次发送回 Master
 * @param  fd_out     int          输出文件描述符，取值范围: >= 0 的可写 fd
 * @param  dir_path   const char*  要扫描的目录路径，不能为空
 * @param  worker_id  int          Worker 编号（当前未使用，保留用于日志），取值范围: >= 0
 * @param  task       WorkerThreadCtx*  线程上下文（用于进度心跳），不能为空
 * @return void
 *
 * @note   先对目录本身执行 lstat 获取设备号；然后 opendir/readdir 遍历条目。
 *         对每个条目：跳过 . 和 ..；尝试 blind-trust；失败则执行 lstat/stat；
 *         收集到 batch_size 条后发送批次；遍历结束后发送剩余批次（或空批次）。
 *         若 opendir 或 lstat 失败，发送错误通知和空批次。
 *         v15.5.3: readdir 循环中每 1000 个条目更新一次 last_progress，
 *         防止大目录（7万+ 文件）遍历被 IPC 线程误判为 stuck。
 *         v15.5.7: 完整性加固——
 *         (1) 条目级 lstat/stat 失败不再静默跳过：ENOENT/ENOTDIR 视为并发删除
 *             竞态静默，其余 errno 经 IPC_MSG_ENTRY_ERROR 上报熔断清单；
 *         (2) 路径截断（>4096）以 ENAMETOOLONG 上报；
 *         (3) readdir 循环每次调用前清零 errno，循环结束后检查——readdir 中途
 *             失败（NFS readdir cookie 失效等）此前完全静默，会造成超大目录
 *             部分条目丢失，现按目录级错误上报；
 *         (4) 条目 stat 带 EINTR 重试（最多 3 次）。
 *         v15.5.8: nlink oracle（--strict-nlink 门控）——readdir 无 errno 假空/假 EOF
 *         的唯一客户端可检旁证：st_nlink-2 与实际子目录计数不符时以
 *         errno_code=0 的 ENTRY_ERROR 上报（NLINK_MISMATCH）。
 */
static void scan_and_send(int fd_out, const char *dir_path, int worker_id, WorkerThreadCtx *task) {
    struct stat dir_st;
    if (lstat(dir_path, &dir_st) != 0) {
        log_warn("[W%d-Scanner] lstat failed on %s: %s", worker_id, dir_path, strerror(errno));
        send_error_and_empty_batch(fd_out, task->fd_ctrl, errno, task->current_dev, dir_path);
        return;
    }

    uint64_t dir_dev = dir_st.st_dev;
    /* v15.5.6: 记录当前任务真实设备号，供 DEV_TIMEOUT 准确上报 */
    task->current_dev = dir_st.st_dev;

    int batch_size = 1024;
    if (g_worker_cfg && g_worker_cfg->batch_size > 0)
        batch_size = g_worker_cfg->batch_size;

    char **paths = calloc(batch_size, sizeof(char*));
    struct stat *stats = calloc(batch_size, sizeof(struct stat));
    int count = 0;

    DIR *dir = opendir(dir_path);
    /* v15.5.9: opendir 本身在 NFS 上就是多个 RPC，可能极慢，
     * 先 tick 一次防止 opendir 期间被误判卡死 */
    time_t last_tick_time = time(NULL);
    scanner_progress_tick(&last_tick_time, 5, &task->progress_mutex, &task->last_progress, worker_id);

    if (!dir) {
        log_warn("[W%d-Scanner] opendir failed on %s: %s", worker_id, dir_path, strerror(errno));
        send_error_and_empty_batch(fd_out, task->fd_ctrl, errno, dir_dev, dir_path);
        goto cleanup;
    }
    log_debug_v(202605181600UL, "[W%d-Scanner] opendir success: %s", worker_id, dir_path);

    struct dirent *entry;
    int entry_count = 0;
    int readdir_err = 0;
    unsigned long subdir_count = 0; /* v15.5.8: nlink oracle 直接子目录计数 */
    int entry_anomalies = 0;        /* v15.5.8: 条目级异常计数（非零时禁用 nlink oracle 防误报） */
    for (;;) {
        errno = 0; /* v15.5.7: 区分 readdir 正常结束与中途出错 */
        entry = readdir(dir);
        if (!entry) {
            readdir_err = errno;
            break;
        }
        /* v15.5.9: 时间驱动 heartbeat tick（每 5 秒），替代计数驱动的 1000 条目间隔 */
        scanner_progress_tick(&last_tick_time, 5,
                              &task->progress_mutex, &task->last_progress,
                              worker_id);

        if (entry->d_name[0] == '.' &&
            (entry->d_name[1] == '\0' || (entry->d_name[1] == '.' && entry->d_name[2] == '\0'))) {
            continue;
        }

        char full_path[4096];
        int n = snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);
        if (n >= (int)sizeof(full_path)) {
            /* v15.5.7: 路径截断不再静默跳过，上报熔断清单 */
            send_entry_error(task->fd_ctrl, ENAMETOOLONG, dir_dev, dir_path);
            continue;
        }

        struct stat st;
        bool got = false;

        if (try_blind_trust(full_path, dir_dev, entry->d_ino, entry->d_type, &st)) {
            got = true;
        } else {
            if (entry_stat(full_path, &st) != 0) {
                /* v15.5.7: 条目级 stat 失败不再静默跳过。
                 * ENOENT/ENOTDIR 为 readdir 后条目被并发删除/替换的正常竞态，静默；
                 * 其余 errno（EACCES/EIO/ETIMEDOUT/ESTALE 等）意味着真实存在的条目
                 * 被遗漏，上报 Master 记入熔断清单并累加 skipped_count。 */
                if (errno != ENOENT && errno != ENOTDIR)
                    send_entry_error(task->fd_ctrl, errno, dir_dev, full_path);
                entry_anomalies++; /* v15.5.8: 条目异常时禁用 nlink oracle，防并发删除误报 */
                continue;
            }
            got = true;
        }

        if (got) {
            if (S_ISDIR(st.st_mode)) subdir_count++; /* v15.5.8: nlink oracle */
            paths[count] = strdup(full_path);
            stats[count] = st;
            count++;
        }

        if (count >= batch_size) {
            /* v15.5.9: send_batch 前 tick，防止 IPC 阻塞期间误判 */
            scanner_progress_tick(&last_tick_time, 5, &task->progress_mutex, &task->last_progress, worker_id);
            send_batch(fd_out, paths, stats, count);
            /* v15.5.9: send_batch 后 tick，长耗时 IPC 已结束 */
            scanner_progress_tick(&last_tick_time, 5, &task->progress_mutex, &task->last_progress, worker_id);
            for (int i = 0; i < count; i++) free(paths[i]);
            count = 0;
        }
    }

    if (readdir_err != 0) {
        /* v15.5.7: readdir 中途失败——目录部分条目可能已丢失。
         * 先 flush 已收集的有效条目，再按目录级错误上报（空批次保证计数平衡）。 */
        log_warn("[W%d-Scanner] readdir failed mid-way on %s: %s", worker_id, dir_path, strerror(readdir_err));
        if (count > 0) {
            send_batch(fd_out, paths, stats, count);
            for (int i = 0; i < count; i++) free(paths[i]);
            count = 0;
        }
        closedir(dir);
        send_error_and_empty_batch(fd_out, task->fd_ctrl, readdir_err, dir_dev, dir_path);
        goto cleanup;
    }

    /* v15.5.8: nlink oracle（--strict-nlink 门控，默认关）。
     * POSIX: 非空目录的 st_nlink = 2 + 直接子目录数。readdir 全程无 errno 的
     * "假空/假 EOF"（NFS 协议层静默截断）不产生任何错误码，唯一可客户端观测的
     * 旁证就是子目录计数与 st_nlink-2 不符。捕获后以 errno_code=0 的
     * ENTRY_ERROR 上报（Master 侧显示 NLINK_MISMATCH）。
     * 默认关闭的原因：NFS/btrfs 等文件系统 nlink 语义不可靠，且扫描期间目录
     * 被并发增删子目录会造成误报；entry_anomalies/readdir_err 非零时同样禁用。 */
    if (g_worker_cfg && g_worker_cfg->strict_nlink && entry_anomalies == 0
        && dir_st.st_nlink >= 2
        && (unsigned long)(dir_st.st_nlink - 2) != subdir_count) {
        log_warn("[W%d-Scanner] NLINK_MISMATCH on %s: st_nlink=%lu (expect %lu subdirs) but readdir saw %lu",
                 worker_id, dir_path, (unsigned long)dir_st.st_nlink,
                 (unsigned long)(dir_st.st_nlink - 2), subdir_count);
        send_entry_error(task->fd_ctrl, 0 /* NLINK_MISMATCH */, dir_dev, dir_path);
    }

    if (count > 0) {
        log_debug_v(202605181600UL, "[W%d-Scanner] sending final batch (count=%d)", worker_id, count);
        send_batch(fd_out, paths, stats, count);
        for (int i = 0; i < count; i++) free(paths[i]);
    } else {
        /* Empty directory: send empty batch so Master decrements pending_tasks */
        log_debug_v(202605181600UL, "[W%d-Scanner] empty dir, sending empty batch", worker_id);
        send_batch(fd_out, NULL, NULL, 0);
    }

    log_debug_v(202605181600UL, "[W%d-Scanner] readdir loop done (entries=%d)", worker_id, entry_count);
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
        scan_and_send(ctx->fd_data, path, ctx->worker_id, ctx);

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
                    log_warn_v(202607030000UL, "[W%d-Scanner] IPC_MSG_FINISH EAGAIN retry %d", ctx->worker_id, retry);
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

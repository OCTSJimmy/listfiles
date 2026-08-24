/**
 * @file worker_proc.c
 * @brief Worker 进程池管理与 Worker 子进程主入口
 *
 * Master 侧：创建、销毁、替换 Worker 子进程，管理双向管道与预备役池。
 * Worker 侧：worker_main 入口，创建 Scanner 线程，维护 IPC 心跳循环。
 *
 * v15.6.1（P0-107/P0-108）：
 * - 全部 fork 集中在单线程期（初始 Worker + 预备役 spare 池），运行期零 fork；
 *   严格超售（vm.overcommit_memory=2）下 fork 按全额 VSZ 计 commit，Master 大 VSZ
 *   时运行期 fork 必败且原实现完全静默（生产事故 R8，54h 空转）。
 * - spawn/fork 失败路径全部全局 log_error。
 */
#define _GNU_SOURCE
#include "worker_proc.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>
#include <sys/wait.h>
#include <pthread.h>
#include <poll.h>

/* ================================================================
 * Worker process entry (v14.0.0: IPC thread + Scanner thread)
 * ================================================================ */

/**
 * @brief  Worker 子进程主入口函数 (v14.0.0 多线程版)
 * @param  fd_cmd     int  Master→Worker 命令通道 fd (SCAN / STOP)
 * @param  fd_data    int  Worker→Master 数据通道 fd (BATCH)
 * @param  fd_ctrl    int  Worker→Master 控制通道 fd (HEARTBEAT / ERROR / EXIT / FINISH)
 * @param  worker_id  int  Worker 编号
 * @return void
 *
 * @note   内部拆分为两条线程：
 *         - Scanner 线程：专职执行 readdir/lstat 等阻塞 IO（worker_scanner_thread）
 *         - IPC 线程（本函数，即主线程）：专职维护 fd_cmd/fd_ctrl 通信与心跳
 *         fd_cmd 设为非阻塞，主线程通过 poll(5s) 循环同时处理：
 *         读任务、发心跳、响应 STOP。Scanner 卡住不影响心跳。
 *         v15.6.1：入口第一时间切到无锁日志模式（fork 子进程可能继承被持有的
 *         stdio 锁，flockfile 会永久死锁——生产事故 R7）。
 */
void worker_main(int fd_cmd, int fd_data, int fd_ctrl, int worker_id) {
    /* v15.6.1（P0-107c）：必须是子进程的第一个动作，早于任何可能打日志的调用 */
    log_set_forked_child();

    /* 设置 fd_cmd 为非阻塞，使 IPC 线程可用 poll 循环 */
    int flags = fcntl(fd_cmd, F_GETFL);
    if (flags >= 0) {
        fcntl(fd_cmd, F_SETFL, flags | O_NONBLOCK);
    }

    WorkerThreadCtx ctx = {
        .fd_cmd = fd_cmd,
        .fd_data = fd_data,
        .fd_ctrl = fd_ctrl,
        .worker_id = worker_id,
        .task_ready = false,
        .stop_flag = false,
        .last_progress = time(NULL),
        .scanner_active = false,
        .current_dev = 0,
        .dev_timeout_reported = false,
    };
    pthread_mutex_init(&ctx.task_mutex, NULL);
    pthread_cond_init(&ctx.task_cond, NULL);
    pthread_mutex_init(&ctx.progress_mutex, NULL);

    pthread_t scanner_tid;
    if (pthread_create(&scanner_tid, NULL, worker_scanner_thread, &ctx) != 0) {
        log_error("[Worker-%d] Failed to create scanner thread", worker_id);
        ipc_send(fd_ctrl, IPC_MSG_EXIT, NULL, 0);
        return;
    }

    /* 发送 READY 信号，通知 Master 初始化完成 */
    int rc_ready = ipc_send(fd_ctrl, IPC_MSG_READY, NULL, 0);
    log_debug("[Worker-%d] READY sent (rc=%d)", worker_id, rc_ready);

    log_info("[Worker-%d] Started, cfg=%p, hb_timeout=%d",
             worker_id, (void*)worker_get_config(),
             worker_get_config() ? worker_get_config()->heartbeat_timeout : -1);

    struct pollfd pfd = { fd_cmd, POLLIN, 0 };
    time_t last_heartbeat = time(NULL);
    int heartbeat_count = 0;

    while (!ctx.stop_flag) {
        time_t now = time(NULL);
        int elapsed = (int)difftime(now, last_heartbeat);
        int timeout_ms = (elapsed >= 5) ? 0 : (5 - elapsed) * 1000;

        int rc = poll(&pfd, 1, timeout_ms);
        if (rc < 0) {
            if (errno == EINTR) continue;
            log_warn("[Worker-%d] poll error: %s", worker_id, strerror(errno));
            break;
        }

        if (rc == 0 || elapsed >= 5) {
            heartbeat_count++;
            if (ctx.scanner_active) {
                log_info_v(202607030000UL, "[Worker-%d] Scanner active (heartbeat %d)", worker_id, heartbeat_count);
            }
            IpcHeartbeatPayload hb = { (uint64_t)time(NULL) };
            int rc_hb = ipc_send(fd_ctrl, IPC_MSG_HEARTBEAT, &hb, sizeof(hb));
            if (heartbeat_count <= 3 || rc_hb != 0) {
                log_debug_v(202605150000UL, "[Worker-%d] heartbeat sent (rc=%d, count=%d)", worker_id, rc_hb, heartbeat_count);
            }
            last_heartbeat = time(NULL);
        }

        if (pfd.revents & POLLIN) {
            IpcMessageHeader hdr;
            rc = ipc_recv_header(fd_cmd, &hdr);
            if (rc == -2) {
                usleep(1000);
                continue;
            }
            if (rc != 0) {
                log_warn("[Worker-%d] recv_header failed: %d, exiting", worker_id, rc);
                break;
            }

            if (hdr.msg_type == IPC_MSG_STOP) {
                log_debug("[Worker-%d] received STOP", worker_id);
                if (hdr.payload_len > 0) {
                    void *tmp = malloc(hdr.payload_len);
                    if (tmp) { ipc_recv_payload(fd_cmd, tmp, hdr.payload_len); free(tmp); }
                }
                ctx.stop_flag = true;
                pthread_mutex_lock(&ctx.task_mutex);
                pthread_cond_signal(&ctx.task_cond);
                pthread_mutex_unlock(&ctx.task_mutex);
                break;
            }

            if (hdr.msg_type != IPC_MSG_SCAN) {
                log_debug("[Worker-%d] unexpected msg_type=%d, dropping", worker_id, hdr.msg_type);
                if (hdr.payload_len > 0) {
                    void *tmp = malloc(hdr.payload_len);
                    if (tmp) { ipc_recv_payload(fd_cmd, tmp, hdr.payload_len); free(tmp); }
                }
                continue;
            }

            log_debug("[Worker-%d] received SCAN (payload_len=%u)", worker_id, hdr.payload_len);

            char *dir_path = malloc(hdr.payload_len + 1);
            if (!dir_path) {
                /* v15.6.1（P0-106）：原为静默 break——Worker 无声退出是生产事故 R9 的
                 * 掩盖层之一，退出出口必须全部有声 */
                log_error("[Worker-%d] malloc(%u) failed for SCAN path, exiting",
                          worker_id, hdr.payload_len + 1);
                break;
            }
            if (ipc_recv_payload(fd_cmd, dir_path, hdr.payload_len) != 0) {
                log_warn("[Worker-%d] SCAN payload recv failed, exiting", worker_id);
                free(dir_path);
                break;
            }
            dir_path[hdr.payload_len] = '\0';

            /* v15.6.0: SCAN payload = IpcScanHeader(epoch) + path */
            uint64_t task_epoch = 0;
            const char *scan_path = dir_path;
            if (hdr.payload_len >= sizeof(IpcScanHeader)) {
                IpcScanHeader sh;
                memcpy(&sh, dir_path, sizeof(sh));
                task_epoch = sh.epoch;
                scan_path = dir_path + sizeof(IpcScanHeader);
            } else {
                log_warn("[Worker-%d] SCAN payload too short (%u), missing epoch header",
                         worker_id, hdr.payload_len);
            }

            pthread_mutex_lock(&ctx.task_mutex);
            strncpy(ctx.task_path, scan_path, sizeof(ctx.task_path) - 1);
            ctx.task_path[sizeof(ctx.task_path) - 1] = '\0';
            ctx.task_epoch = task_epoch;
            ctx.dev_timeout_reported = false; /* v15.6.1（P0-102）：新任务复位节流标志 */
            ctx.task_ready = true;
            pthread_cond_signal(&ctx.task_cond);
            pthread_mutex_unlock(&ctx.task_mutex);

            free(dir_path);
        }

        if (pfd.revents & (POLLERR | POLLHUP)) {
            /* v15.6.1（P0-106）：原为静默 break——Master 侧管道关闭即 Worker 被判弃，
             * 必须留痕 */
            log_warn("[Worker-%d] fd_cmd POLLERR/POLLHUP (revents=0x%x), exiting",
                     worker_id, pfd.revents);
            break;
        }
        /* Scanner progress timeout check */
        const Config *cfg = worker_get_config();
        if (cfg && ctx.scanner_active) {
            pthread_mutex_lock(&ctx.progress_mutex);
            time_t scanner_last = ctx.last_progress;
            pthread_mutex_unlock(&ctx.progress_mutex);
            int timeout_sec = cfg->heartbeat_timeout > 0
                              ? cfg->heartbeat_timeout
                              : HEARTBEAT_TIMEOUT_SEC;
            if (difftime(now, scanner_last) > timeout_sec) {
                /* v15.6.1（P0-102）：同一任务只报一次 DEV_TIMEOUT——原实现每 5s 重发，
                 * Master 对每条都 cleanup，换代后滞留消息对新一代 slot 重复销账，
                 * pending_tasks 被打成负数（生产事故 R2） */
                if (!ctx.dev_timeout_reported) {
                    ctx.dev_timeout_reported = true;
                    /* v15.6.1（P0-106）：解除版本门控——scanner 卡死意味着该目录元数据
                     * 可能丢失，按既定规则必须全局可见 */
                    log_error("[Worker-%d] Scanner stuck for %ds on %s, reporting to master",
                              worker_id, timeout_sec, ctx.task_path);
                    IpcErrorHeader eh = { ETIMEDOUT, (uint64_t)ctx.current_dev };
                    char stuck_path[4096];
                    pthread_mutex_lock(&ctx.task_mutex);
                    strncpy(stuck_path, ctx.task_path, sizeof(stuck_path) - 1);
                    stuck_path[sizeof(stuck_path) - 1] = '\0';
                    pthread_mutex_unlock(&ctx.task_mutex);
                    uint32_t plen = (uint32_t)strlen(stuck_path);
                    size_t err_total = sizeof(eh) + sizeof(plen) + plen;
                    uint8_t *err_buf = malloc(err_total);
                    if (err_buf) {
                        memcpy(err_buf, &eh, sizeof(eh));
                        memcpy(err_buf + sizeof(eh), &plen, sizeof(plen));
                        memcpy(err_buf + sizeof(eh) + sizeof(plen), stuck_path, plen);
                        ipc_send(fd_ctrl, IPC_MSG_DEV_TIMEOUT, err_buf, (uint32_t)err_total);
                        free(err_buf);
                    }
                }
            }
        }
    }

    /* 通知 Scanner 停止并等待其结束 */
    pthread_mutex_lock(&ctx.task_mutex);
    ctx.stop_flag = true;
    pthread_cond_signal(&ctx.task_cond);
    pthread_mutex_unlock(&ctx.task_mutex);
    pthread_join(scanner_tid, NULL);

    ipc_send(fd_ctrl, IPC_MSG_EXIT, NULL, 0);

    pthread_mutex_destroy(&ctx.task_mutex);
    pthread_cond_destroy(&ctx.task_cond);
    pthread_mutex_destroy(&ctx.progress_mutex);
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
    atomic_store(&pool->active_count, 0);
    return pool;
}

/**
 * @brief  销毁 Worker 进程池并清理所有资源
 * @param  pool  WorkerPool*  要销毁的进程池指针，允许传入 NULL（空操作）
 * @return void
 *
 * @note   对存活的 Worker 与全部预备役发送 SIGKILL（不阻塞等待，避免 D-State 挂起），
 *         关闭所有管道 fd，释放 backlog_paths 中的路径内存。
 *         最后以非阻塞方式收割所有僵尸子进程（waitpid(-1, WNOHANG)）。
 */
void worker_pool_destroy(WorkerPool *pool) {
    if (!pool) return;
    for (int i = 0; i < pool->num_workers; i++) {
        WorkerSlot *slot = &pool->slots[i];
        if (atomic_load(&slot->is_alive)) {
            kill(slot->pid, SIGKILL);
            close(slot->fd_cmd);
            if (slot->fd_cmd_rd >= 0) close(slot->fd_cmd_rd);
            close(slot->fd_data);
            close(slot->fd_ctrl);
        }
        /* Free backlog paths */
        for (int j = 0; j < slot->backlog_count; j++) {
            free(slot->backlog_paths[j]);
        }
        free(slot->backlog_paths);
    }
    /* v15.6.1（P0-107b）：清理预备役——未启用的 spare 也是子进程，不得留孤儿 */
    for (int i = 0; i < pool->spare_count; i++) {
        SpareWorker *sp = &pool->spares[i];
        if (sp->pid > 0) kill(sp->pid, SIGKILL);
        if (sp->fd_cmd >= 0) close(sp->fd_cmd);
        if (sp->fd_cmd_rd >= 0) close(sp->fd_cmd_rd);
        if (sp->fd_data >= 0) close(sp->fd_data);
        if (sp->fd_ctrl >= 0) close(sp->fd_ctrl);
    }
    free(pool->spares);
    /* Non-blocking reap of any zombie children */
    for (int i = 0; i < (pool->num_workers + pool->spare_total) * 3; i++) {
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
 * @brief  创建一组 Worker 管道并 fork 一个子进程（内部原语，v15.6.1 抽取）
 * @param  worker_id     int     子进程日志用编号（初始 Worker 为 slot 号，spare 为 -1）
 * @param  out_fd_cmd_w  int*    输出：Master 写端（M→W 命令）
 * @param  out_fd_cmd_rd int*    输出：Master 侧保留的读端（cleanup drain 用）
 * @param  out_fd_data_r int*    输出：Master 读端（W→M BATCH）
 * @param  out_fd_ctrl_r int*    输出：Master 读端（W→M 控制信号）
 * @param  out_pid       pid_t*  输出：子进程 pid
 * @return bool  成功 true；失败 false（已全局 log_error，含 errno）
 *
 * @note   v15.6.1（P0-108）：所有失败路径必须全局留痕——fork ENOMEM 曾在生产
 *         环境静默失败 54 小时（严格超售下按全额 VSZ 计 commit）。
 *         v15.6.1（P0-107a）：本原语只允许在单线程期调用。
 */
static bool spawn_one_worker(int worker_id, int *out_fd_cmd_w, int *out_fd_cmd_rd,
                             int *out_fd_data_r, int *out_fd_ctrl_r, pid_t *out_pid) {
    int cmd_pipe[2], data_pipe[2], ctrl_pipe[2];
    if (pipe2(cmd_pipe, O_CLOEXEC) != 0) {
        log_error("[Spawn] pipe2(cmd) failed: errno=%d (%s)", errno, strerror(errno));
        return false;
    }
    if (pipe2(data_pipe, O_CLOEXEC) != 0) {
        log_error("[Spawn] pipe2(data) failed: errno=%d (%s)", errno, strerror(errno));
        close(cmd_pipe[0]); close(cmd_pipe[1]);
        return false;
    }
    if (pipe2(ctrl_pipe, O_CLOEXEC) != 0) {
        log_error("[Spawn] pipe2(ctrl) failed: errno=%d (%s)", errno, strerror(errno));
        close(cmd_pipe[0]); close(cmd_pipe[1]);
        close(data_pipe[0]); close(data_pipe[1]);
        return false;
    }

    /* Enlarge pipe buffers to reduce deadlock probability */
    enlarge_pipe(cmd_pipe[0]);  enlarge_pipe(cmd_pipe[1]);
    enlarge_pipe(data_pipe[0]); enlarge_pipe(data_pipe[1]);
    enlarge_pipe(ctrl_pipe[0]); enlarge_pipe(ctrl_pipe[1]);

    /* Master read ends must be non-blocking for IPC thread epoll responsiveness */
    int data_flags = fcntl(data_pipe[0], F_GETFL);
    if (data_flags >= 0) {
        fcntl(data_pipe[0], F_SETFL, data_flags | O_NONBLOCK);
    } else {
        log_warn("[Spawn] fcntl(F_GETFL) on fd_data failed: errno=%d", errno);
    }
    int ctrl_flags = fcntl(ctrl_pipe[0], F_GETFL);
    if (ctrl_flags >= 0) {
        fcntl(ctrl_pipe[0], F_SETFL, ctrl_flags | O_NONBLOCK);
    } else {
        log_warn("[Spawn] fcntl(F_GETFL) on fd_ctrl failed: errno=%d", errno);
    }

    /* Worker write ends must be non-blocking to prevent bidirectional pipe deadlock */
    int wdata_flags = fcntl(data_pipe[1], F_GETFL);
    if (wdata_flags >= 0) {
        fcntl(data_pipe[1], F_SETFL, wdata_flags | O_NONBLOCK);
    } else {
        log_warn("[Spawn] fcntl(F_GETFL) on fd_data_wr failed: errno=%d", errno);
    }
    int wctrl_flags = fcntl(ctrl_pipe[1], F_GETFL);
    if (wctrl_flags >= 0) {
        fcntl(ctrl_pipe[1], F_SETFL, wctrl_flags | O_NONBLOCK);
    } else {
        log_warn("[Spawn] fcntl(F_GETFL) on fd_ctrl_wr failed: errno=%d", errno);
    }

    pid_t pid = fork();
    if (pid < 0) {
        /* v15.6.1（P0-108）：fork 失败必须全局留痕。ENOMEM 多为严格超售下
         * VSZ 过大——请检查 vm.overcommit_memory 与 --estimated-files */
        log_error("[Spawn] fork failed: errno=%d (%s)", errno, strerror(errno));
        close(cmd_pipe[0]); close(cmd_pipe[1]);
        close(data_pipe[0]); close(data_pipe[1]);
        close(ctrl_pipe[0]); close(ctrl_pipe[1]);
        return false;
    }

    if (pid == 0) {
        /* Child */
        close(cmd_pipe[1]);
        close(data_pipe[0]);
        close(ctrl_pipe[0]);

        /* Close all inherited fds except our pipes */
        int max_fd = (int)sysconf(_SC_OPEN_MAX);
        if (max_fd < 0) max_fd = 65536;
        for (int fd = 3; fd < max_fd; fd++) {
            if (fd != cmd_pipe[0] && fd != data_pipe[1] && fd != ctrl_pipe[1]) {
                close(fd);
            }
        }

        worker_main(cmd_pipe[0], data_pipe[1], ctrl_pipe[1], worker_id);
        _exit(0);
    }

    /* Parent */
    /* 注意：保留 cmd_pipe[0] 给 cleanup_dead_worker_slot drain 用，不要在这里关闭 */
    close(data_pipe[1]);
    close(ctrl_pipe[1]);

    /* Master write end must be non-blocking to prevent bidirectional pipe deadlock */
    int flags = fcntl(cmd_pipe[1], F_GETFL);
    if (flags >= 0) {
        fcntl(cmd_pipe[1], F_SETFL, flags | O_NONBLOCK);
    } else {
        log_warn("[Spawn] fcntl(F_GETFL) on fd_cmd failed: errno=%d", errno);
    }

    *out_fd_cmd_w  = cmd_pipe[1];
    *out_fd_cmd_rd = cmd_pipe[0];
    *out_fd_data_r = data_pipe[0];
    *out_fd_ctrl_r = ctrl_pipe[0];
    *out_pid = pid;
    return true;
}

/**
 * @brief  将 fork 出的子进程装入 slot（内部辅助，v15.6.1 抽取）
 * @note   spawn 与 spare 启用共用；初始化 slot 全部运行时字段，
 *         新 Worker 无在途任务，旧 epoch 的残留消息必被丢弃。
 */
static void slot_attach(WorkerPool *pool, int slot_id, pid_t pid,
                        int fd_cmd_w, int fd_cmd_rd, int fd_data_r, int fd_ctrl_r) {
    WorkerSlot *slot = &pool->slots[slot_id];
    slot->pid = pid;
    slot->fd_cmd = fd_cmd_w;
    slot->fd_cmd_rd = fd_cmd_rd;
    slot->fd_data = fd_data_r;
    slot->fd_ctrl = fd_ctrl_r;
    atomic_store(&slot->is_alive, true);
    atomic_store(&slot->state, WORKER_STATE_INITIALIZING);  /* v15.1.1: spawn 初始为 INITIALIZING */
    atomic_store(&slot->last_heartbeat, time(NULL));
    slot->current_dev = 0;
    slot->current_epoch = 0;  /* v15.6.0: 新 Worker 无在途任务，使旧 epoch 的残留消息必被丢弃 */
    slot->current_path[0] = '\0';
    slot->backlog_paths = NULL;
    slot->backlog_count = 0;
    slot->backlog_capacity = 0;
    atomic_flag_clear(&slot->cleanup_done);
    atomic_fetch_add(&pool->active_count, 1);
}

/**
 * @brief  在指定 slot 中 fork 一个新的 Worker 子进程（仅限单线程启动期调用）
 * @param  pool     WorkerPool*  目标进程池指针，不能为空
 * @param  slot_id  int          目标 slot 索引，取值范围: [0, pool->num_workers-1]
 * @return bool  返回 true 表示 fork 成功；false 表示失败（管道创建失败或 fork 失败，
 *               已全局 log_error——v15.6.1 P0-108）
 *
 * @note   v15.6.1（P0-107a）：本函数只允许在单线程启动期调用；运行期替换一律
 *         走 worker_pool_replace 的 spare 池，运行期禁止 fork。
 */
bool worker_pool_spawn(WorkerPool *pool, int slot_id) {
    pid_t pid;
    int fd_cmd_w, fd_cmd_rd, fd_data_r, fd_ctrl_r;
    if (!spawn_one_worker(slot_id, &fd_cmd_w, &fd_cmd_rd, &fd_data_r, &fd_ctrl_r, &pid)) {
        return false;
    }
    slot_attach(pool, slot_id, pid, fd_cmd_w, fd_cmd_rd, fd_data_r, fd_ctrl_r);
    return true;
}

/**
 * @brief  fork 预备役 Worker 池（仅限单线程启动期调用，v15.6.1 P0-107b）
 * @param  pool   WorkerPool*  目标进程池指针，不能为空
 * @param  count  int          spare 数量（建议 = num_workers）
 * @return bool   true = 至少一个 spare 就绪；false = 全部失败
 *
 * @note   spare 是完整的 Worker 子进程（READY/心跳写入管道缓冲，被启用前无人读取，
 *         缓冲可承载数年心跳）。单个 spare fork 失败即停止继续 fork（连续失败大概率
 *         是资源级问题），缩减 spare 数并全局告警；spare 为空时运行期无法替换死亡
 *         Worker，扫描将降额运行（不致命）。
 */
bool worker_pool_spawn_spares(WorkerPool *pool, int count) {
    if (!pool || count <= 0) return false;
    pool->spares = calloc((size_t)count, sizeof(SpareWorker));
    if (!pool->spares) {
        log_error("[Spare] calloc(%d) failed，预备役为空", count);
        return false;
    }
    int ok = 0;
    for (int i = 0; i < count; i++) {
        SpareWorker *sp = &pool->spares[ok];
        if (!spawn_one_worker(-1, &sp->fd_cmd, &sp->fd_cmd_rd, &sp->fd_data, &sp->fd_ctrl, &sp->pid)) {
            log_error("[Spare] 第 %d/%d 个 spare fork 失败，预备役缩减为 %d 个", i + 1, count, ok);
            break;
        }
        ok++;
    }
    pool->spare_count = ok;
    pool->spare_total = ok;
    if (ok == 0) {
        log_error("[Spare] 预备役为空：运行期 Worker 死亡将无法替换，扫描将降额运行");
    } else {
        log_info("[Spare] 预备役 Worker %d 个就绪", ok);
    }
    return ok > 0;
}

/**
 * @brief  替换指定 slot 中的 Worker 子进程（杀死旧进程并启用 spare）
 * @param  pool     WorkerPool*  目标进程池指针，不能为空
 * @param  slot_id  int          目标 slot 索引，取值范围: [0, pool->num_workers-1]
 * @return bool  返回 true 表示替换成功；false 表示无 spare 可用（已全局 log_error，
 *               扫描降额运行——v15.6.1 P0-108）
 *
 * @note   对存活的旧 Worker 发送 SIGKILL，随后轮询 waitpid(WNOHANG) 确认旧进程
 *         已被回收（每次间隔 1ms，最多 ~100ms；超时记 WARN 后继续），
 *         避免其 Scanner 线程在内核完成的残留 write 与新 Worker 数据串扰。
 *         v15.6.1（P0-107b）：替换不再 fork，改从预备役池取启动期预 fork 的
 *         spare——运行期零 fork，彻底消除严格超售下 fork ENOMEM 与多线程
 *         fork 锁继承两类风险。
 */
bool worker_pool_replace(WorkerPool *pool, int slot_id) {
    WorkerSlot *slot = &pool->slots[slot_id];
    if (atomic_load(&slot->is_alive)) {
        pid_t old_pid = slot->pid;
        kill(old_pid, SIGKILL);
        /* v15.6.0: SIGKILL 后轮询 waitpid 确认旧进程真正回收（通常 < 1ms），
         * 旧进程可能处于 D-State 不可杀死，超时记 WARN 后继续 */
        int st;
        int waited_ms = 0;
        while (waitpid(old_pid, &st, WNOHANG) == 0 && waited_ms < 100) {
            usleep(1000);
            waited_ms++;
        }
        if (waited_ms >= 100) {
            log_warn("[Replace] waitpid timeout for worker %d (pid=%d), continuing anyway",
                     slot_id, (int)old_pid);
        }
        close(slot->fd_cmd);
        if (slot->fd_cmd_rd >= 0) {
            close(slot->fd_cmd_rd);
            slot->fd_cmd_rd = -1;
        }
        close(slot->fd_data);
        close(slot->fd_ctrl);
        atomic_store(&slot->is_alive, false);
        atomic_store(&slot->state, WORKER_STATE_DEAD);  /* v15.1.0 */
        atomic_fetch_sub(&pool->active_count, 1);
    }

    /* v15.6.1（P0-107b/P0-108）：spare 耗尽只告警一次（主循环每 100ms 重试，
     * 不节流会刷屏）；耗尽后扫描以剩余 Worker 降额运行，由有效进展看门狗兜底 */
    if (pool->spare_count <= 0) {
        if (!pool->spare_exhausted_logged) {
            pool->spare_exhausted_logged = true;
            log_error("[Replace] 预备役 Worker 已耗尽，slot %d 无法替换，扫描降额运行", slot_id);
        }
        return false;
    }
    SpareWorker sp = pool->spares[--pool->spare_count];
    log_warn("[Replace] slot %d 启用预备役 Worker (pid=%d)，剩余 spare %d/%d",
             slot_id, (int)sp.pid, pool->spare_count, pool->spare_total);
    slot_attach(pool, slot_id, sp.pid, sp.fd_cmd, sp.fd_cmd_rd, sp.fd_data, sp.fd_ctrl);
    return true;
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
            int rc = ipc_send(pool->slots[i].fd_cmd, IPC_MSG_STOP, NULL, 0);
            (void)rc; /* STOP is best-effort; fd_cmd may be non-blocking */
        }
    }
}

/**
 * @file worker_proc.c
 * @brief Worker 进程池管理与 Worker 子进程主入口
 *
 * Master 侧：创建、销毁、替换 Worker 子进程，管理双向管道。
 * Worker 侧：worker_main 入口，创建 Scanner 线程，维护 IPC 心跳循环。
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
 */
void worker_main(int fd_cmd, int fd_data, int fd_ctrl, int worker_id) {
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
                log_info("[Worker-%d] Scanner active (heartbeat %d)", worker_id, heartbeat_count);
            }
            IpcHeartbeatPayload hb = { (uint64_t)time(NULL) };
            int rc_hb = ipc_send(fd_ctrl, IPC_MSG_HEARTBEAT, &hb, sizeof(hb));
            if (heartbeat_count <= 3 || rc_hb != 0) {
                log_debug_v(202605150000, "[Worker-%d] heartbeat sent (rc=%d, count=%d)", worker_id, rc_hb, heartbeat_count);
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
                log_warn("[Worker-%d] recv_header failed: %d", worker_id, rc);
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
            if (!dir_path) break;
            if (ipc_recv_payload(fd_cmd, dir_path, hdr.payload_len) != 0) {
                free(dir_path);
                break;
            }
            dir_path[hdr.payload_len] = '\0';

            pthread_mutex_lock(&ctx.task_mutex);
            strncpy(ctx.task_path, dir_path, sizeof(ctx.task_path) - 1);
            ctx.task_path[sizeof(ctx.task_path) - 1] = '\0';
            ctx.task_ready = true;
            pthread_cond_signal(&ctx.task_cond);
            pthread_mutex_unlock(&ctx.task_mutex);

            free(dir_path);
        }

        if (pfd.revents & (POLLERR | POLLHUP)) {
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
                log_error("[Worker-%d] Scanner stuck for %ds on %s, reporting to master",
                          worker_id, timeout_sec, ctx.task_path);
                IpcErrorHeader eh = { ETIMEDOUT, 0 };
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
 * @note   对存活的 Worker 发送 SIGKILL（不阻塞等待，避免 D-State 挂起），
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
    int cmd_pipe[2], data_pipe[2], ctrl_pipe[2];
    if (pipe2(cmd_pipe, O_CLOEXEC) != 0) return false;
    if (pipe2(data_pipe, O_CLOEXEC) != 0) {
        close(cmd_pipe[0]); close(cmd_pipe[1]);
        return false;
    }
    if (pipe2(ctrl_pipe, O_CLOEXEC) != 0) {
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
        log_warn("[worker_pool_spawn] fcntl(F_GETFL) on fd_data failed: errno=%d", errno);
    }
    int ctrl_flags = fcntl(ctrl_pipe[0], F_GETFL);
    if (ctrl_flags >= 0) {
        fcntl(ctrl_pipe[0], F_SETFL, ctrl_flags | O_NONBLOCK);
    } else {
        log_warn("[worker_pool_spawn] fcntl(F_GETFL) on fd_ctrl failed: errno=%d", errno);
    }

    /* Worker write ends must be non-blocking to prevent bidirectional pipe deadlock */
    int wdata_flags = fcntl(data_pipe[1], F_GETFL);
    if (wdata_flags >= 0) {
        fcntl(data_pipe[1], F_SETFL, wdata_flags | O_NONBLOCK);
    } else {
        log_warn("[worker_pool_spawn] fcntl(F_GETFL) on fd_data_wr failed: errno=%d", errno);
    }
    int wctrl_flags = fcntl(ctrl_pipe[1], F_GETFL);
    if (wctrl_flags >= 0) {
        fcntl(ctrl_pipe[1], F_SETFL, wctrl_flags | O_NONBLOCK);
    } else {
        log_warn("[worker_pool_spawn] fcntl(F_GETFL) on fd_ctrl_wr failed: errno=%d", errno);
    }

    pid_t pid = fork();
    if (pid < 0) {
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

        worker_main(cmd_pipe[0], data_pipe[1], ctrl_pipe[1], slot_id);
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
        log_warn("[worker_pool_spawn] fcntl(F_GETFL) on fd_cmd failed: errno=%d", errno);
    }

    WorkerSlot *slot = &pool->slots[slot_id];
    slot->pid = pid;
    slot->fd_cmd = cmd_pipe[1];
    slot->fd_cmd_rd = cmd_pipe[0];
    slot->fd_data = data_pipe[0];
    slot->fd_ctrl = ctrl_pipe[0];
    atomic_store(&slot->is_alive, true);
    atomic_store(&slot->state, WORKER_STATE_INITIALIZING);  /* v15.1.1: spawn 初始为 INITIALIZING */
    atomic_store(&slot->last_heartbeat, time(NULL));
    slot->current_dev = 0;
    slot->current_path[0] = '\0';
    slot->backlog_paths = NULL;
    slot->backlog_count = 0;
    slot->backlog_capacity = 0;
    atomic_flag_clear(&slot->cleanup_done);
    atomic_fetch_add(&pool->active_count, 1);
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
    if (atomic_load(&slot->is_alive)) {
        kill(slot->pid, SIGKILL);
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
            int rc = ipc_send(pool->slots[i].fd_cmd, IPC_MSG_STOP, NULL, 0);
            (void)rc; /* STOP is best-effort; fd_cmd may be non-blocking */
        }
    }
}

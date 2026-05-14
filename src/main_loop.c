/**
 * @file main_loop.c
 * @brief Master 进程 epoll 主循环与 IPC 消息处理
 *
 * 采用 epoll_wait + eventfd 的事件驱动架构：
 * - 监听所有 Worker 的 fd_out，接收 BATCH / HEARTBEAT / ERROR / EXIT 消息
 * - 监听 eventfd，接收线程池完成通知
 * - CPU 密集型去重 offload 到 ThreadPool（默认 4 线程）
 * - 支持 Worker 积压队列（backlog）避免双向管道死锁
 * - 支持历史 pbin 目录泵送（恢复模式）
 * - 自动替换心跳超时的 Worker
 */
#define _GNU_SOURCE
#include "main_loop.h"
#include "utils.h"
#include "progress.h"
#include "worker_proc.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/wait.h>
#include <signal.h>
#include <stdatomic.h>
#include <dirent.h>
#include <poll.h>

#define EVENTFD_SLOT_ID 0xFFFFFFFFU

/**
 * @brief  安全地接收 IPC 消息头部，带 100ms poll 超时防止永久阻塞
 * @param  fd   int                 源文件描述符
 * @param  hdr  IpcMessageHeader*   输出缓冲区指针
 * @return int  返回 0 成功；-1 错误/EOF；-2 超时（poll 100ms 无数据）
 *
 * @note   先 poll 检查 fd 是否可读，带 100ms 超时。若超时返回 -2，让调用方继续 epoll 循环。
 *         这完全消除了 fd 号重用、Worker 半死等场景下的永久阻塞风险。
 */
static int safe_ipc_recv_header(int fd, IpcMessageHeader *hdr) {
    struct pollfd pfd = { fd, POLLIN, 0 };
    int rc = poll(&pfd, 1, 100);  /* 100ms timeout */
    if (rc == 0) {
        /* Timeout: no data available, safe to skip */
        return -2;
    }
    if (rc < 0) {
        if (errno == EINTR) return -2;  /* Interrupted, try again next loop */
        fprintf(stderr, "[IPC] poll error on fd=%d: errno=%d (%s)\n",
                fd, errno, strerror(errno));
        return -1;
    }
    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
        fprintf(stderr, "[IPC] poll error/hup on fd=%d (revents=0x%x)\n",
                fd, pfd.revents);
        return -1;
    }
    /* Data is available, do the actual read */
    return ipc_recv_header(fd, hdr);
}

/**
 * @brief  安全地接收 IPC 消息负载，带 100ms poll 超时
 * @param  fd   int    源文件描述符
 * @param  buf  void*  负载接收缓冲区
 * @param  len  uint32_t  要接收的字节数
 * @return int  返回 0 成功；-1 错误；-2 超时
 */
static int safe_ipc_recv_payload(int fd, void *buf, uint32_t len) {
    if (len == 0) return 0;
    size_t nread = 0;
    while (nread < len) {
        struct pollfd pfd = { fd, POLLIN, 0 };
        int rc = poll(&pfd, 1, 100);
        if (rc == 0) return -2;  /* Timeout */
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
        if (n == 0) return -1;  /* EOF */
        nread += n;
    }
    return 0;
}

/**
 * @brief  统一清理死亡 Worker 的资源并修正 pending_tasks
 * @param  ctx       AppContext*  应用上下文
 * @param  worker_id int          Worker 编号
 * @return void
 *
 * @note   本函数用于所有 Worker 死亡路径（monitor timeout、epoll error、normal exit）。
 *         会 drain fd_in_rd 中未读的 SCAN 任务，将 backlog 迁移到 lost_tasks，
 *         关闭所有 fd，从 epoll 移除，并精确调整 pending_tasks。
 *         幂等设计：重复调用同一 slot 安全（检查 fd >= 0）。
 */
void cleanup_dead_worker_slot(AppContext *ctx, int worker_id) {
    if (!ctx || !ctx->worker_pool) return;
    if (worker_id < 0 || worker_id >= ctx->worker_pool->num_workers) return;
    WorkerSlot *slot = &ctx->worker_pool->slots[worker_id];
    if (!slot->is_alive && slot->pid == -1) return; /* already cleaned */
    if (atomic_flag_test_and_set(&slot->cleanup_done)) return; /* already cleaned by another path */

    /* Drain fd_in_rd to count orphaned SCAN tasks */
    int orphaned = 0;
    if (slot->fd_in_rd >= 0) {
        orphaned = ipc_drain_and_count_tasks(slot->fd_in_rd);
        if (orphaned > 0) {
            fprintf(stderr, "[Cleanup] Worker %d drained %d orphaned tasks from fd_in_rd\n", worker_id, orphaned);
        }
        close(slot->fd_in_rd);
        slot->fd_in_rd = -1;
    }

    /* Migrate backlog to lost_tasks */
    pthread_mutex_lock(&ctx->lost_tasks_mutex);
    for (int j = 0; j < slot->backlog_count; j++) {
        char *path = slot->backlog_paths[j];
        if (!path) continue;
        if (ctx->lost_count >= ctx->lost_capacity) {
            size_t new_cap = ctx->lost_capacity ? ctx->lost_capacity * 2 : 64;
            char **new_arr = realloc(ctx->lost_tasks, new_cap * sizeof(char *));
            if (new_arr) {
                ctx->lost_tasks = new_arr;
                ctx->lost_capacity = new_cap;
            }
        }
        if (ctx->lost_count < ctx->lost_capacity) {
            ctx->lost_tasks[ctx->lost_count++] = path;
        } else {
            fprintf(stderr, "[Warning] Lost task queue full, dropping %s\n", path);
            free(path);
            atomic_fetch_sub(&ctx->pending_tasks, 1);
        }
    }
    pthread_mutex_unlock(&ctx->lost_tasks_mutex);
    free(slot->backlog_paths);
    slot->backlog_paths = NULL;
    slot->backlog_count = 0;
    slot->backlog_capacity = 0;

    /* Close write-end fd_in */
    if (slot->fd_in >= 0) {
        close(slot->fd_in);
        slot->fd_in = -1;
    }

    /* Remove from epoll and close read-end fd_out */
    if (ctx->epfd >= 0 && slot->fd_out >= 0) {
        epoll_ctl(ctx->epfd, EPOLL_CTL_DEL, slot->fd_out, NULL);
    }
    if (slot->fd_out >= 0) {
        close(slot->fd_out);
        slot->fd_out = -1;
    }

    /* Adjust counters: 1 for the current task + orphaned from fd_in_rd */
    atomic_fetch_sub(&ctx->pending_tasks, 1 + orphaned);

    /* Mark dead */
    if (slot->is_alive) {
        slot->is_alive = false;
        atomic_fetch_sub(&ctx->worker_pool->active_count, 1);
    }
    slot->pid = -1;
}

/* ================================================================
 * Batch payload parser
 * ================================================================ */

/**
 * @brief  解析后的 Worker 批次数据结构
 *
 * 从 IPC_MSG_BATCH 的 payload 中解析出的路径数组和对应的 stat 数组。
 * 由 parse_batch 函数填充，parsed_batch_free 函数释放。
 */
typedef struct {
    char **paths;
    struct stat *stats;
    int count;
} ParsedBatch;

/**
 * @brief  释放 ParsedBatch 内部分配的内存
 * @param  b  ParsedBatch*  指向要释放的批次结构，允许传入 NULL（空操作）
 * @return void
 */
static void parsed_batch_free(ParsedBatch *b) {
    if (!b) return;
    for (int i = 0; i < b->count; i++) free(b->paths[i]);
    free(b->paths);
    free(b->stats);
    b->paths = NULL;
    b->stats = NULL;
    b->count = 0;
}

/**
 * @brief  从 IPC payload 中解析出批次数据
 * @param  payload  const uint8_t*  IPC 消息负载缓冲区指针，不能为空
 * @param  len      uint32_t        负载总长度（字节），取值范围: >= sizeof(IpcBatchHeader)
 * @param  out      ParsedBatch*    输出结构体指针，用于存放解析结果，不能为空
 * @return bool  返回 true 表示解析成功；false 表示格式错误或内存不足
 *
 * @note   解析格式：IpcBatchHeader(count) + count * ([uint32_t plen][char path[plen]][struct stat st])。
 *         所有路径字符串均通过 malloc 独立分配，以 '\0' 结尾。
 *         解析失败时会自动释放已分配的内存，避免泄漏。
 */
static bool parse_batch(const uint8_t *payload, uint32_t len, ParsedBatch *out) {
    memset(out, 0, sizeof(*out));
    if (len < sizeof(IpcBatchHeader)) return false;

    const uint8_t *p = payload;
    IpcBatchHeader bh;
    memcpy(&bh, p, sizeof(bh));
    p += sizeof(bh);

    out->paths = calloc(bh.count, sizeof(char*));
    out->stats = calloc(bh.count, sizeof(struct stat));
    if (!out->paths || !out->stats) goto fail;

    for (uint32_t i = 0; i < bh.count; i++) {
        if ((size_t)(p - payload) + sizeof(uint32_t) > len) goto fail;
        uint32_t plen;
        memcpy(&plen, p, sizeof(plen));
        p += sizeof(plen);

        if ((size_t)(p - payload) + plen + sizeof(struct stat) > len) goto fail;

        out->paths[i] = malloc(plen + 1);
        if (!out->paths[i]) goto fail;
        memcpy(out->paths[i], p, plen);
        out->paths[i][plen] = '\0';
        p += plen;

        memcpy(&out->stats[i], p, sizeof(struct stat));
        p += sizeof(struct stat);
        out->count++;
    }
    return true;

fail:
    parsed_batch_free(out);
    return false;
}

/* ================================================================
 * Thread pool callback: CPU-intensive deduplication
 * ================================================================ */

/**
 * @brief  线程池工作回调：对一批文件执行指纹计算、去重与黑名单检查
 * @param  batch      TPBatch*  要处理的批次，包含 paths、stats、count、results 数组，不能为空
 * @param  user_data  void*     用户数据指针，实际为 AppContext*，不能为空
 * @return void
 *
 * @note   对 batch 中每个条目：
 *         1. 调用 fp_compute 计算 128-bit 指纹
 *         2. 调用 fp_set_insert 尝试插入 visited_set，若已存在则 result |= 1（重复）
 *         3. 调用 dev_mgr_is_blacklisted 检查设备状态，若已熔断则 result |= 2（黑名单）
 *         结果写入 batch->results[i]，由主线程在 process_completed_batch 中读取。
 *         本函数在线程池工作线程中执行，不访问主线程的 epoll 状态。
 */
static void batch_dedup_worker(TPBatch *batch, void *user_data) {
    AppContext *ctx = user_data;
    for (int i = 0; i < batch->count; i++) {
        const char *path = batch->paths[i];
        struct stat *st = &batch->stats[i];
        uint8_t fp[FP_SIZE];
        fp_compute(path, st->st_dev, st->st_ino, fp);
        uint8_t result = 0;
        if (fp_set_insert(ctx->visited_set, fp)) {
            result |= 1; /* duplicate */
        }
        if (dev_mgr_is_blacklisted(ctx->dev_mgr, st->st_dev)) {
            result |= 2; /* blacklisted */
        }
        batch->results[i] = result;
    }
}

/* ================================================================
 * Side effects for a completed batch (must run on main thread)
 * ================================================================ */

/**
 * @brief  处理线程池完成的一个批次（必须在主线程执行，含副作用操作）
 * @param  ctx    AppContext*  应用上下文指针，不能为空
 * @param  batch  TPBatch*     已完成的批次，包含去重结果，不能为空
 * @return void
 *
 * @note   主线程副作用处理流程：
 *         1. 跳过重复项（result & 1）和黑名单项（result & 2）
 *         2. 对目录：
 *            - 若处于 HIST_PUMP_OLD 阶段，新子目录追加到 fpbin（不直接入队）
 *            - 否则增加 pending_tasks，通过 ipc_send 分发到 Worker；若返回 EAGAIN(-2) 则缓存到 backlog
 *            - 若开启 --dirs，将目录本身加入输出批次
 *            - 若开启 --print-dir，打印目录路径到 dir_info_fp
 *            - 若处于续传模式且非 HIST_PUMP_OLD，追加到 record_path 缓冲
 *         3. 对文件：加入输出批次，若续传模式追加到 record_path 缓冲
 *         4. 输出批次达到 ASYNC_BATCH_SIZE 时批量提交到 async_writer
 *         5. 最后递减 pending_tasks 和 pending_batches，释放 batch 内存
 */
static void process_completed_batch(AppContext *ctx, TPBatch *batch) {
    OutputBatch out_batch = {0};
    
    for (int i = 0; i < batch->count; i++) {
        const char *path = batch->paths[i];
        struct stat *st = &batch->stats[i];
        uint8_t result = batch->results[i];
        
        if (result & 1) continue; /* duplicate */
        if (result & 2) {
            ctx->state.has_error = true;
            continue; /* blacklisted */
        }
        
        if (S_ISDIR(st->st_mode)) {
            if (ctx->hist_pump_state == HIST_PUMP_OLD) {
                fpbin_append(ctx, path, st);
            } else {
                atomic_fetch_add(&ctx->pending_tasks, 1);
                uint32_t plen = (uint32_t)strlen(path);
                /* [FIX] 轮询分发任务，避免所有任务堆积在单个 Worker 上 */
                int wid = ctx->next_dispatch_worker % ctx->worker_pool->num_workers;
                ctx->next_dispatch_worker++;
                WorkerSlot *slot = &ctx->worker_pool->slots[wid];
                if (!slot->is_alive) {
                    fprintf(stderr, "[Dispatch] WARNING: selected dead worker %d (path=%s), skipping\n", wid, path);
                    atomic_fetch_sub(&ctx->pending_tasks, 1);
                    continue;
                }
                slot->current_dev = st->st_dev;
                safe_strcpy(slot->current_path, path, sizeof(slot->current_path));
                int rc = ipc_send(slot->fd_in, IPC_MSG_SCAN, path, plen);
                if (rc == -1) {
                    fprintf(stderr, "[Dispatch] ipc_send to worker %d failed: errno=%d (%s), path=%s\n",
                            wid, errno, strerror(errno), path);
                    atomic_fetch_sub(&ctx->pending_tasks, 1);
                }
                if (rc == -2) {
                    /* fd_in is full (EAGAIN); enqueue to backlog to avoid deadlock */
                    if (slot->backlog_count >= slot->backlog_capacity) {
                        int new_cap = slot->backlog_capacity ? slot->backlog_capacity * 2 : 64;
                        char **new_arr = realloc(slot->backlog_paths, new_cap * sizeof(char *));
                        if (new_arr) {
                            slot->backlog_paths = new_arr;
                            slot->backlog_capacity = new_cap;
                        }
                    }
                    if (slot->backlog_count < slot->backlog_capacity) {
                        slot->backlog_paths[slot->backlog_count++] = strdup(path);
                    } else {
                        /* Backlog full: drop task (rare with 1MB pipe buffer) */
                        atomic_fetch_sub(&ctx->pending_tasks, 1);
                        fprintf(stderr, "[Warning] Worker %d backlog full, dropping task %s\n", wid, path);
                    }
                }
            }
            
            ctx->state.dir_count++;
            if (ctx->cfg.include_dir) {
                OutputTask *task = calloc(1, sizeof(OutputTask));
                task->path = strdup(path);
                task->st = *st;
                if (out_batch.tail) {
                    out_batch.tail->next = task;
                } else {
                    out_batch.head = task;
                }
                out_batch.tail = task;
                out_batch.count++;
            }
            if (ctx->cfg.print_dir && ctx->state.dir_info_fp && !ctx->cfg.mute) {
                fprintf(ctx->state.dir_info_fp, "%s%s\n", OUTPUT_DIR_PREFIX, path);
            }
            if (ctx->cfg.continue_mode && ctx->hist_pump_state != HIST_PUMP_OLD) {
                record_path_batch_append(&ctx->cfg, &ctx->state, &ctx->record_batch, path, st);
            }
        } else {
            ctx->state.file_count++;
            OutputTask *task = calloc(1, sizeof(OutputTask));
            task->path = strdup(path);
            task->st = *st;
            if (out_batch.tail) {
                out_batch.tail->next = task;
            } else {
                out_batch.head = task;
            }
            out_batch.tail = task;
            out_batch.count++;
            if (ctx->cfg.continue_mode) {
                record_path_batch_append(&ctx->cfg, &ctx->state, &ctx->record_batch, path, st);
            }
        }
        
        if (out_batch.count >= ASYNC_BATCH_SIZE) {
            async_writer_submit_batch(ctx->async_writer, &out_batch);
            out_batch.head = NULL;
            out_batch.tail = NULL;
            out_batch.count = 0;
        }
    }
    
    if (out_batch.count > 0) {
        async_writer_submit_batch(ctx->async_writer, &out_batch);
    }
    
    atomic_fetch_sub(&ctx->pending_tasks, 1);
    atomic_fetch_sub(&ctx->pending_batches, 1);
    ctx->state.total_dequeued_count++;
    
    /* 释放 batch 内存 */
    for (int i = 0; i < batch->count; i++) free(batch->paths[i]);
    free(batch->paths);
    free(batch->stats);
    free(batch->results);
    free(batch);
}

/**
 * @brief  将 Worker 积压队列中的任务刷出到对应 Worker（非阻塞写重试）
 * @param  ctx  AppContext*  应用上下文指针，不能为空
 * @return void
 *
 * @note   遍历所有 WorkerSlot，对存活的 Worker 尝试逐条发送 backlog_paths 中缓存的任务。
 *         若某条任务仍遇到 EAGAIN，则停止该 Worker 的刷出，留待下次循环重试。
 *         发送成功的任务会 free 并置 NULL，最后压缩数组去除空洞。
 *         在主循环每轮 epoll 事件处理结束后调用，作为死锁预防机制。
 */
static void flush_worker_backlogs(AppContext *ctx) {
    for (int i = 0; i < ctx->worker_pool->num_workers; i++) {
        WorkerSlot *slot = &ctx->worker_pool->slots[i];
        if (!slot->is_alive || slot->backlog_count == 0) continue;
        
        int sent = 0;
        for (int j = 0; j < slot->backlog_count; j++) {
            char *path = slot->backlog_paths[j];
            uint32_t plen = (uint32_t)strlen(path);
            int rc = ipc_send(slot->fd_in, IPC_MSG_SCAN, path, plen);
            if (rc == -2) break; /* Still full, stop and retry next cycle */
            if (rc == -1) {
                fprintf(stderr, "[Backlog] ipc_send failed for %s, dropping\n", path);
                atomic_fetch_sub(&ctx->pending_tasks, 1);
                free(path);
                slot->backlog_paths[j] = NULL;
                sent++;
                continue;
            }
        }
        
        /* Compact array: move remaining items to front */
        if (sent > 0) {
            int new_count = 0;
            for (int j = 0; j < slot->backlog_count; j++) {
                if (slot->backlog_paths[j]) {
                    slot->backlog_paths[new_count++] = slot->backlog_paths[j];
                }
            }
            slot->backlog_count = new_count;
        }
    }
}

/**
 * @brief  轮询并处理线程池中所有已完成的 batch
 * @param  ctx  AppContext*  应用上下文指针，不能为空
 * @return void
 *
 * @note   反复调用 thread_pool_poll_completed 直到返回 NULL，
 *         对每个完成的 batch 调用 process_completed_batch 执行主线程副作用。
 *         本函数在 epoll eventfd 事件触发后和主循环每轮末尾调用。
 */
/**
 * @brief  将丢失的任务重新分发给 Worker
 * @param  ctx  AppContext*  应用上下文指针，不能为空
 * @return void
 *
 * @note   Worker 超时死亡时，其正在处理的任务会被保存到 lost_tasks 队列。
 *         本函数在 epoll 循环中定期调用，将丢失的任务重新轮询分发给存活的 Worker。
 *         若 ipc_send 返回 EAGAIN，则任务保留在队列中等待下次重试。
 */
static void dispatch_lost_tasks(AppContext *ctx) {
    if (ctx->lost_count == 0) return;

    pthread_mutex_lock(&ctx->lost_tasks_mutex);
    size_t dispatched = 0;
    for (size_t i = 0; i < ctx->lost_count; i++) {
        char *path = ctx->lost_tasks[i];
        if (!path) continue;
        
        /* 轮询选择存活 Worker */
        int wid = ctx->next_dispatch_worker % ctx->worker_pool->num_workers;
        ctx->next_dispatch_worker++;
        WorkerSlot *slot = &ctx->worker_pool->slots[wid];
        if (!slot->is_alive) {
            fprintf(stderr, "[LostTasks] skip dead worker %d (path=%s)\n", wid, path);
            continue;
        }
        
        uint32_t plen = (uint32_t)strlen(path);
        int rc = ipc_send(slot->fd_in, IPC_MSG_SCAN, path, plen);
        if (rc == -2) {
            /* fd_in 满，停止分发，留待下次循环 */
            break;
        }
        if (rc == -1) {
            fprintf(stderr, "[LostTasks] ipc_send failed for %s, dropping\n", path);
            atomic_fetch_sub(&ctx->pending_tasks, 1);
            free(path);
            ctx->lost_tasks[i] = NULL;
            dispatched++;
            continue;
        }
        
        /* 发送成功 */
        slot->current_dev = 0;  /* 未知，将在 Worker 心跳中更新 */
        safe_strcpy(slot->current_path, path, sizeof(slot->current_path));
        free(path);
        ctx->lost_tasks[i] = NULL;
        dispatched++;
    }
    
    /* 压缩数组 */
    if (dispatched > 0) {
        size_t new_count = 0;
        for (size_t i = 0; i < ctx->lost_count; i++) {
            if (ctx->lost_tasks[i]) {
                ctx->lost_tasks[new_count++] = ctx->lost_tasks[i];
            }
        }
        ctx->lost_count = new_count;
    }
    pthread_mutex_unlock(&ctx->lost_tasks_mutex);
}

static void drain_completed_batches(AppContext *ctx) {
    TPBatch *batch;
    while ((batch = thread_pool_poll_completed(ctx->thread_pool)) != NULL) {
        process_completed_batch(ctx, batch);
    }
}

/* ================================================================
 * Main loop: epoll event loop
 * ================================================================ */

/**
 * @brief  运行 Master 主事件循环（epoll_wait 驱动）
 * @param  ctx  AppContext*  应用上下文指针，不能为空
 * @return void
 *
 * @note   完整循环流程：
 *         1. 将所有 Worker 的 fd_out 和 eventfd 注册到 epoll
 *         2. epoll_wait 500ms 超时循环
 *         3. 处理 Worker 消息：BATCH → 提交线程池；HEARTBEAT → 更新心跳时间；
 *            ERROR → 设备熔断处理；EXIT → Worker 正常退出标记
 *         4. 处理 eventfd： drain_completed_batches
 *         5. 泵送历史 pbin 目录（恢复模式）
 *         6. 替换已标记死亡的 Worker
 *         7. 刷出 backlog
 *         8. 终止条件：pending_tasks == 0 && pending_batches == 0 && 无活跃恢复
 *         退出前刷出所有完成 batch 和 record_path 缓冲。
 */
void main_loop_run(AppContext *ctx) {
    ctx->epfd = epoll_create1(EPOLL_CLOEXEC);
    if (ctx->epfd < 0) {
        perror("epoll_create1");
        return;
    }

    struct epoll_event ev;
    for (int i = 0; i < ctx->worker_pool->num_workers; i++) {
        memset(&ev, 0, sizeof(ev));
        ev.events = EPOLLIN | EPOLLERR | EPOLLHUP;
        ev.data.u32 = (uint32_t)i;
        if (epoll_ctl(ctx->epfd, EPOLL_CTL_ADD, ctx->worker_pool->slots[i].fd_out, &ev) != 0) {
            perror("epoll_ctl");
        }
    }

    /* 创建 eventfd 用于线程池完成通知 */
    ctx->event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (ctx->event_fd < 0) {
        perror("eventfd");
        close(ctx->epfd);
        ctx->epfd = -1;
        return;
    }
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.u32 = EVENTFD_SLOT_ID;
    if (epoll_ctl(ctx->epfd, EPOLL_CTL_ADD, ctx->event_fd, &ev) != 0) {
        perror("epoll_ctl eventfd");
    }

    /* 创建线程池 */
    ctx->thread_pool = thread_pool_create(ctx->cfg.master_threads, ctx->event_fd,
                                          batch_dedup_worker, ctx);
    if (!ctx->thread_pool) {
        fprintf(stderr, "[Fatal] 无法创建线程池\n");
        close(ctx->event_fd);
        ctx->event_fd = -1;
        close(ctx->epfd);
        ctx->epfd = -1;
        return;
    }

    struct epoll_event events[64];
    ctx->running = true;

    while (ctx->running) {
        int nfds = epoll_wait(ctx->epfd, events, 64, 500); /* 500ms timeout */

        for (int i = 0; i < nfds; i++) {
            uint32_t slot_id = events[i].data.u32;
            if (slot_id == EVENTFD_SLOT_ID) {
                uint64_t n;
                (void)read(ctx->event_fd, &n, sizeof(n));
                drain_completed_batches(ctx);
                continue;
            }
            if (slot_id >= (uint32_t)ctx->worker_pool->num_workers) continue;

            /* Handle EPOLLERR/EPOLLHUP directly: worker fd is broken */
            if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                fprintf(stderr, "[Epoll] Worker %u fd error/hup (events=0x%x, fd=%d). Mark dead. active=%d->%d\n",
                        slot_id, events[i].events, ctx->worker_pool->slots[slot_id].fd_out,
                        atomic_load(&ctx->worker_pool->active_count), atomic_load(&ctx->worker_pool->active_count) - 1);
                cleanup_dead_worker_slot(ctx, (int)slot_id);
                continue;
            }

            /* Skip events from workers already marked dead by monitor */
            if (!ctx->worker_pool->slots[slot_id].is_alive) continue;

            IpcMessageHeader hdr;
            int rc_hdr = safe_ipc_recv_header(ctx->worker_pool->slots[slot_id].fd_out, &hdr);
            if (rc_hdr == -2) {
                /* Timeout: data not yet available, skip and wait for next epoll */
                continue;
            }
            if (rc_hdr != 0) {
                /* Worker pipe broken, mark dead and clean up fd */
                fprintf(stderr, "[Epoll] Worker %u recv_header failed (events=0x%x, fd=%d). Mark dead. active=%d->%d\n",
                        slot_id, events[i].events, ctx->worker_pool->slots[slot_id].fd_out,
                        atomic_load(&ctx->worker_pool->active_count), atomic_load(&ctx->worker_pool->active_count) - 1);
                cleanup_dead_worker_slot(ctx, (int)slot_id);
                continue;
            }

            if (hdr.payload_len > 16 * 1024 * 1024) {
                /* Sanity check: refuse huge payload */
                void *tmp = malloc(hdr.payload_len);
                if (tmp) { safe_ipc_recv_payload(ctx->worker_pool->slots[slot_id].fd_out, tmp, hdr.payload_len); free(tmp); }
                continue;
            }

            void *payload = NULL;
            if (hdr.payload_len > 0) {
                payload = malloc(hdr.payload_len);
                int rc_payload = safe_ipc_recv_payload(ctx->worker_pool->slots[slot_id].fd_out, payload, hdr.payload_len);
                if (rc_payload != 0) {
                    free(payload);
                    if (rc_payload == -2) {
                        /* Partial payload timeout: data may be incomplete, skip */
                        fprintf(stderr, "[Epoll] Worker %u payload timeout (fd=%d, len=%u). Skip.\n",
                                slot_id, ctx->worker_pool->slots[slot_id].fd_out, hdr.payload_len);
                    } else {
                        fprintf(stderr, "[Epoll] Worker %u payload recv failed (fd=%d). Mark dead.\n",
                                slot_id, ctx->worker_pool->slots[slot_id].fd_out);
                        cleanup_dead_worker_slot(ctx, (int)slot_id);
                    }
                    continue;
                }
            }

            switch (hdr.msg_type) {
                case IPC_MSG_BATCH:
                    main_loop_handle_batch(ctx, (int)slot_id, payload, hdr.payload_len);
                    break;
                case IPC_MSG_HEARTBEAT: {
                    if (hdr.payload_len >= sizeof(IpcHeartbeatPayload)) {
                        IpcHeartbeatPayload *hb = (IpcHeartbeatPayload*)payload;
                        main_loop_handle_heartbeat(ctx, (int)slot_id, hb->timestamp);
                    }
                    break;
                }
                case IPC_MSG_ERROR: {
                    if (hdr.payload_len >= sizeof(IpcErrorHeader)) {
                        IpcErrorHeader *eh = (IpcErrorHeader*)payload;
                        const char *path = "";
                        if (hdr.payload_len > sizeof(IpcErrorHeader)) {
                            path = (const char*)payload + sizeof(IpcErrorHeader) + sizeof(uint32_t);
                        }
                        main_loop_handle_error(ctx, (int)slot_id, eh, path);
                    }
                    break;
                }
                case IPC_MSG_EXIT:
                    main_loop_handle_exit(ctx, (int)slot_id);
                    break;
            }

            free(payload);
        }

        /* Drain any completed batches before checking termination */
        drain_completed_batches(ctx);

        /* Pump historical pbin directories during recovery */
        if (ctx->hist_pump_state == HIST_PUMP_OLD || ctx->hist_pump_state == HIST_PUMP_NEW) {
            pump_pbin_batch(ctx, ctx->cfg.batch_size);
        }

        /* Reap any zombie children first */
        for (int i = 0; i < ctx->worker_pool->num_workers * 2; i++) {
            if (waitpid(-1, NULL, WNOHANG) <= 0) break;
        }

        /* Replace dead workers */
        for (int i = 0; i < ctx->worker_pool->num_workers; i++) {
            WorkerSlot *slot = &ctx->worker_pool->slots[i];
            if (!slot->is_alive && slot->pid == -1) {
                /* Ensure cleanup has been performed (idempotent if already done) */
                cleanup_dead_worker_slot(ctx, i);
                
                fprintf(stderr, "[Replace] Replacing dead worker %d\n", i);
                worker_pool_replace(ctx->worker_pool, i);
                /* Re-add new fd_out to epoll */
                memset(&ev, 0, sizeof(ev));
                ev.events = EPOLLIN | EPOLLERR | EPOLLHUP;
                ev.data.u32 = (uint32_t)i;
                epoll_ctl(ctx->epfd, EPOLL_CTL_ADD, slot->fd_out, &ev);
            }
        }

        /* Dispatch lost tasks (from timed-out workers) */
        dispatch_lost_tasks(ctx);
        
        /* Flush any backlogged tasks to workers (deadlock prevention) */
        flush_worker_backlogs(ctx);

        /* Termination condition */
        if (atomic_load(&ctx->pending_tasks) == 0 && !ctx->resume_active
            && atomic_load(&ctx->pending_batches) == 0) {
            /* pending_tasks 和 pending_batches 均为 0，说明所有工作已完成 */
            worker_pool_stop_all(ctx->worker_pool);
            ctx->running = false;
        }
    }

    /* 退出前刷出所有已完成的 batch */
    drain_completed_batches(ctx);
    /* 刷出残留的 record_path 缓冲 */
    record_path_batch_flush(&ctx->cfg, &ctx->state, &ctx->record_batch);

    close(ctx->epfd);
    ctx->epfd = -1;
}

/* ================================================================
 * Message handlers
 * ================================================================ */

/**
 * @brief  处理 Worker 发送的 BATCH 消息
 * @param  ctx       AppContext*   应用上下文指针，不能为空
 * @param  worker_id int           发送该消息的 Worker 编号，取值范围: [0, num_workers-1]
 * @param  payload   const void*   IPC 消息负载指针，不能为空
 * @param  len       uint32_t      负载长度（字节）
 * @return void
 *
 * @note   先调用 parse_batch 解析负载，然后分配 results 数组并封装为 TPBatch。
 *         尝试提交到线程池；若队列满则降级为同步处理：
 *         直接调用 batch_dedup_worker 执行去重，再调用 process_completed_batch 处理副作用。
 */
void main_loop_handle_batch(AppContext *ctx, int worker_id, const void *payload, uint32_t len) {
    ParsedBatch parsed;
    if (!parse_batch(payload, len, &parsed)) return;

    uint8_t *results = calloc((size_t)parsed.count, 1);
    if (!results) {
        parsed_batch_free(&parsed);
        return;
    }

    TPBatch *batch = malloc(sizeof(TPBatch));
    if (!batch) {
        free(results);
        parsed_batch_free(&parsed);
        return;
    }
    batch->paths = parsed.paths;
    batch->stats = parsed.stats;
    batch->count = parsed.count;
    batch->results = results;
    batch->worker_id = worker_id;

    /* 尝试提交到线程池 */
    atomic_fetch_add(&ctx->pending_batches, 1);
    if (thread_pool_submit(ctx->thread_pool, batch)) {
        /* 提交成功，ParsedBatch 内存所有权转移给 batch */
        return;
    }

    /* 队列满，降级为同步处理 */
    batch_dedup_worker(batch, ctx);
    process_completed_batch(ctx, batch);
}

/**
 * @brief  处理 Worker 发送的心跳消息
 * @param  ctx        AppContext*  应用上下文指针，不能为空
 * @param  worker_id  int          发送心跳的 Worker 编号，取值范围: [0, num_workers-1]
 * @param  timestamp  uint64_t     Worker 发送心跳时的 Unix 时间戳
 * @return void
 *
 * @note   更新对应 WorkerSlot 的 last_heartbeat 字段。
 *         监控线程（monitor）会定期检查该字段，若超时则判定 Worker 卡死并替换。
 */
void main_loop_handle_heartbeat(AppContext *ctx, int worker_id, uint64_t timestamp) {
    if (worker_id < 0 || worker_id >= ctx->worker_pool->num_workers) return;
    ctx->worker_pool->slots[worker_id].last_heartbeat = (time_t)timestamp;
}

/**
 * @brief  处理 Worker 发送的错误消息（设备级故障）
 * @param  ctx       AppContext*         应用上下文指针，不能为空
 * @param  worker_id int                 发送错误的 Worker 编号
 * @param  err       const IpcErrorHeader* 错误头部信息指针，不能为空
 * @param  path      const char*         发生错误的文件/目录路径
 * @return void
 *
 * @note   仅处理 ETIMEDOUT(110) 和 EIO(5) 两种设备级错误：
 *         1. 若设备尚未被标记为 DEAD，则调用 dev_mgr_mark_dead 熔断设备
 *         2. 将错误路径记录到 spbin（跳过记录）
 *         3. 向 ProbeScheduler 推入探测任务，启动渐进恢复流程
 *         若设备已经是 DEAD 状态，则仅记录日志，避免重复操作。
 */
void main_loop_handle_error(AppContext *ctx, int worker_id, const IpcErrorHeader *err, const char *path) {
    (void)worker_id;
    if (err->errno_code == ETIMEDOUT || err->errno_code == EIO) {
        dev_t dev = (dev_t)err->dev;
        fprintf(stderr, "[Monitor] Worker error on dev %lu: %s (errno=%d)\n",
                (unsigned long)dev, path, err->errno_code);
        
        if (dev_mgr_get_state(ctx->dev_mgr, dev) != DEV_STATE_DEAD) {
            dev_mgr_mark_dead(ctx->dev_mgr, dev);
            ctx->state.has_error = true;

            /* Record to spbin */
            SpbinEntry entry = {0};
            entry.path = strdup(path);
            entry.dev = dev;
            entry.blacklist_time = time(NULL);
            entry.retry_count = 0;
            entry.probe_interval = PROBE_INTERVAL_INITIAL;
            entry.d_type = DT_DIR; /* usually directory scan errors */
            entry.s_status = SP_STATUS_PROBING;
            spbin_append(ctx, &entry);

            /* Launch probe */
            ProbeTask task = {0};
            task.dev = dev;
            safe_strcpy(task.probe_path, path, sizeof(task.probe_path));
            task.next_probe_time = time(NULL) + PROBE_INTERVAL_INITIAL;
            task.probe_interval = PROBE_INTERVAL_INITIAL;
            task.retry_count = 0;
            task.s_status = SP_STATUS_PROBING;
            probe_scheduler_push(ctx->probe_scheduler, &task);
        }
    }
}

/**
 * @brief  处理 Worker 正常退出消息（IPC_MSG_EXIT）
 * @param  ctx       AppContext*  应用上下文指针，不能为空
 * @param  worker_id int          发送 EXIT 的 Worker 编号，取值范围: [0, num_workers-1]
 * @return void
 *
 * @note   将 WorkerSlot 标记为 is_alive=false，减少 active_count，
 *         并立即以 WNOHANG 方式收割僵尸进程。
 *         主循环会在后续轮次检测到该 slot 死亡并调用 worker_pool_replace 重新 spawn。
 */
void main_loop_handle_exit(AppContext *ctx, int worker_id) {
    if (worker_id < 0 || worker_id >= ctx->worker_pool->num_workers) return;
    WorkerSlot *slot = &ctx->worker_pool->slots[worker_id];
    fprintf(stderr, "[Exit] Worker %d normal exit (pid=%d). active=%d->%d\n",
            worker_id, slot->pid, atomic_load(&ctx->worker_pool->active_count), atomic_load(&ctx->worker_pool->active_count) - 1);
    /* Reap zombie immediately */
    int status;
    waitpid(slot->pid, &status, WNOHANG);
    cleanup_dead_worker_slot(ctx, worker_id);
}

/**
 * @file monitor.c
 * @brief 监控线程实现
 *
 * 独立的监控线程负责：
 * 1. 每 500ms 刷新一次统计面板（输出到 stdout），显示运行时间、Worker 状态、吞吐量、进度、设备状态等
 * 2. 每 1s 检查一次 Worker 心跳超时，对超时的 Worker 发送 SIGKILL 并标记为死亡
 * 3. 调度敢死队探测进程：对熔断设备 fork 子进程执行 lstat 探测
 * 4. 收割已完成的探测进程，根据退出状态决定设备恢复或重新调度探测
 *
 * 监控面板输出到 stdout，便于用户在终端实时查看进度；其他诊断信息（日志、错误等）统一输出到 stderr。
 */
#define _GNU_SOURCE
#include "monitor.h"
#include "app_context.h"
#include "progress.h"
#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <time.h>
#include <math.h>

/* ================================================================
 * Statistics helpers (ported from monitor.c.bak)
 * ================================================================ */

/**
 * @brief  计算简单平均速率（总数量 / 总耗时）
 * @param  start_time  time_t          任务开始时间戳
 * @param  count       unsigned long   累计数量（目录数或文件数）
 * @return double  平均速率（个/秒）；若耗时小于 1 秒则返回 0.0
 */
static double calculate_rate_simple(time_t start_time, unsigned long count) {
    time_t current_time = time(NULL);
    double elapsed = difftime(current_time, start_time);
    if (elapsed < 1.0) return 0.0;
    return (double)count / elapsed;
}

/**
 * @brief  更新运行时统计状态（滑动窗口速率计算）
 * @param  state  RuntimeState*  运行时状态指针，不能为空
 * @return void
 *
 * @note   使用 RATE_WINDOW_SIZE（60）个采样点的环形数组计算滑动窗口速率。
 *         每秒钟记录一次当前 dir_count、file_count、dequeued_count，
 *         然后计算当前窗口内的平均速率（dir_rate、file_rate、dequeue_rate）。
 *         同时跟踪历史最大速率。
 */
static void update_statistics(RuntimeState *state) {
    Statistics *st = &state->stats;
    time_t now = time(NULL);

    if (now - st->last_sample_time < 1) return;

    st->head_idx = (st->head_idx + 1) % RATE_WINDOW_SIZE;
    if (st->head_idx == 0 && st->samples[0].timestamp != 0) {
        st->filled = true;
    }

    RateSample *new_s = &st->samples[st->head_idx];
    new_s->timestamp = now;
    new_s->dir_count = state->dir_count;
    new_s->file_count = state->file_count;
    new_s->dequeued_count = state->total_dequeued_count;

    st->last_sample_time = now;

    int tail_idx = st->filled ? (st->head_idx + 1) % RATE_WINDOW_SIZE : 0;
    RateSample *old_s = &st->samples[tail_idx];
    double time_diff = difftime(new_s->timestamp, old_s->timestamp);

    if (time_diff >= 1.0) {
        st->current_dir_rate = (double)(new_s->dir_count - old_s->dir_count) / time_diff;
        if (st->current_dir_rate > st->max_dir_rate) st->max_dir_rate = st->current_dir_rate;

        st->current_file_rate = (double)(new_s->file_count - old_s->file_count) / time_diff;
        if (st->current_file_rate > st->max_file_rate) st->max_file_rate = st->current_file_rate;

        st->current_dequeue_rate = (double)(new_s->dequeued_count - old_s->dequeued_count) / time_diff;
        if (st->current_dequeue_rate > st->max_dequeue_rate) st->max_dequeue_rate = st->current_dequeue_rate;
    } else {
        st->current_dir_rate = calculate_rate_simple(state->start_time, state->dir_count);
        st->current_file_rate = calculate_rate_simple(state->start_time, state->file_count);
    }
}

/**
 * @brief  格式化已运行时间为可读字符串
 * @param  start_time  time_t   任务开始时间戳
 * @param  buffer      char*    输出缓冲区，不能为空
 * @param  buf_size    size_t   缓冲区容量，建议 >= 32
 * @return void
 *
 * @note   输出格式示例："1d 05:32:18"
 */
static void format_elapsed_time(time_t start_time, char *buffer, size_t buf_size) {
    long elapsed = difftime(time(NULL), start_time);
    int days = elapsed / 86400; elapsed %= 86400;
    int hours = elapsed / 3600; elapsed %= 3600;
    int minutes = elapsed / 60;
    int seconds = elapsed % 60;
    snprintf(buffer, buf_size, "%dd %02d:%02d:%02d", days, hours, minutes, seconds);
}

/* ================================================================
 * Progress panel output
 * ================================================================ */

/**
 * @brief  打印实时监控面板到 stdout
 * @param  mon  Monitor*  监控器指针，不能为空
 * @return void
 *
 * @note   若 stdout 为终端（isatty），则先清屏（ANSI 转义序列）。
 *         面板内容包括：版本号、运行时间、活跃 Worker 数、待处理任务数、
 *         目录/文件/消费速率、已扫描目录/文件数、输出切片状态、
 *         进度分片状态、设备熔断状态、敢死队探测状态。
 */
void print_progress(Monitor *mon) {
    AppContext *ctx = mon->ctx;
    if (!ctx) return;

    RuntimeState *state = &ctx->state;
    Config *cfg = &ctx->cfg;

    update_statistics(state);

    FILE *fp = stdout;

    char time_str[32];
    format_elapsed_time(state->start_time, time_str, sizeof(time_str));

    int alive_workers = 0;
    int total_workers = 0;
    if (ctx->worker_pool) {
        total_workers = ctx->worker_pool->num_workers;
        for (int i = 0; i < total_workers; i++) {
            if (ctx->worker_pool->slots[i].is_alive) alive_workers++;
        }
    }

    long pending = atomic_load(&ctx->pending_tasks);
    long pending_batches = atomic_load(&ctx->pending_batches);

    if (isatty(fileno(fp))) {
        fprintf(fp, "\033[2J\033[H");
    }

    fprintf(fp, "===== listfiles v%s =====\n", VERSION);
    fprintf(fp, "Elapsed: %s\n", time_str);

    fprintf(fp, "\n[Workers]\n");
    fprintf(fp, "  Active: %d / %d\n", alive_workers, total_workers);
    fprintf(fp, "  Pending tasks: %ld\n", pending);
    fprintf(fp, "  Pending batches: %ld\n", pending_batches);

    fprintf(fp, "\n[Throughput]\n");
    fprintf(fp, "  Dir rate:  %8.2f/s (max: %.2f)\n", state->stats.current_dir_rate, state->stats.max_dir_rate);
    fprintf(fp, "  File rate: %8.2f/s (max: %.2f)\n", state->stats.current_file_rate, state->stats.max_file_rate);
    fprintf(fp, "  Dequeue:   %8.2f/s (max: %.2f)\n", state->stats.current_dequeue_rate, state->stats.max_dequeue_rate);

    fprintf(fp, "\n[Progress]\n");
    fprintf(fp, "  Dirs:  %lu\n", state->dir_count);
    fprintf(fp, "  Files: %lu\n", state->file_count);

    if (cfg->is_output_split_dir) {
        fprintf(fp, "  Output slice: %lu (line: %lu)\n", state->output_slice_num, state->output_line_count);
    }

    if (cfg->continue_mode && cfg->progress_base) {
        fprintf(fp, "  Progress slice: %lu (line: %lu)\n", state->write_slice_index, state->line_count);
    }

    if (ctx->dev_mgr) {
        size_t dev_count = atomic_load(&ctx->dev_mgr->count);
        if (dev_count > 0) {
            int dead = 0, condemned = 0;
            for (size_t i = 0; i < dev_count; i++) {
                DeviceState ds = (DeviceState)atomic_load(&ctx->dev_mgr->entries[i].state);
                if (ds == DEV_STATE_DEAD) dead++;
                if (ds == DEV_STATE_CONDEMNED) condemned++;
            }
            if (dead > 0 || condemned > 0) {
                fprintf(fp, "\n[Devices]\n");
                fprintf(fp, "  Dead: %d, Condemned: %d\n", dead, condemned);
            }
        }
    }

    if (mon->active_probe_pid > 0) {
        fprintf(fp, "\n[Probe] active pid=%d\n", mon->active_probe_pid);
    }

    fprintf(fp, "==========================\n");
    fflush(fp);
}

/* ================================================================
 * Worker health check (directly inspect WorkerPool slots)
 * ================================================================ */

/**
 * @brief  检查所有 Worker 的心跳超时并替换卡死 Worker
 * @param  mon  Monitor*  监控器指针，不能为空
 * @return void
 *
 * @note   遍历 WorkerPool 中所有 is_alive 的 slot，
 *         若当前时间与 last_heartbeat 的差值超过 cfg->heartbeat_timeout（默认 30s），
 *         则发送 SIGKILL，以 WNOHANG 方式 waitpid，标记 slot 为死亡并减少 active_count。
 *         不阻塞等待，因为 Worker 可能处于 D-State 不可杀死。
 */
static void check_workers_health(Monitor *mon) {
    AppContext *ctx = mon->ctx;
    if (!ctx || !ctx->worker_pool) return;

    time_t now = time(NULL);
    int timeout = ctx->cfg.heartbeat_timeout;

    for (int i = 0; i < ctx->worker_pool->num_workers; i++) {
        WorkerSlot *slot = &ctx->worker_pool->slots[i];
        if (!slot->is_alive) continue;

        if (now - slot->last_heartbeat > timeout) {
            char timebuf[32];
            struct tm *tm_info = localtime(&now);
            strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", tm_info);
            fprintf(stderr, "[%s] [Monitor] Worker %d heartbeat timeout (dev=%lu, path=%s). Replacing.\n",
                    timebuf, i, (unsigned long)slot->current_dev, slot->current_path);

            kill(slot->pid, SIGKILL);
            int status;
            waitpid(slot->pid, &status, WNOHANG);

            /* [FIX] 关闭管道 fd，防止主线程继续向 dead worker 写入数据导致任务幽灵化 */
            if (slot->fd_in >= 0) {
                close(slot->fd_in);
                slot->fd_in = -1;
            }
            if (slot->fd_out >= 0) {
                close(slot->fd_out);
                slot->fd_out = -1;
            }

            slot->is_alive = false;
            slot->pid = -1;
            ctx->worker_pool->active_count--;
            ctx->state.has_error = true;
            /* Decrement pending_tasks so the system doesn't wait forever for a dead worker */
            atomic_fetch_sub(&ctx->pending_tasks, 1);
            
            /* [FIX] 将超时 Worker 的当前任务保存到 lost_tasks，稍后重新分发 */
            if (slot->current_path[0] != '\0') {
                if (ctx->lost_count >= ctx->lost_capacity) {
                    size_t new_cap = ctx->lost_capacity ? ctx->lost_capacity * 2 : 64;
                    char **new_arr = realloc(ctx->lost_tasks, new_cap * sizeof(char *));
                    if (new_arr) {
                        ctx->lost_tasks = new_arr;
                        ctx->lost_capacity = new_cap;
                    }
                }
                if (ctx->lost_count < ctx->lost_capacity) {
                    ctx->lost_tasks[ctx->lost_count++] = strdup(slot->current_path);
                    /* pending_tasks 在 monitor 中已减 1，重新分发时会再加 1 */
                    atomic_fetch_add(&ctx->pending_tasks, 1);
                }
            }
        }
    }
}

/* ================================================================
 * Daredevil probe (fork-based, same logic as old main_loop.c)
 * ================================================================ */

/**
 * @brief  调度敢死队探测进程
 * @param  mon  Monitor*  监控器指针，不能为空
 * @return void
 *
 * @note   从 ProbeScheduler 中 peek 一个到期的探测任务：
 *         - 若任务状态为 CONDEMNED，直接移除不再探测
 *         - 否则 fork 子进程，子进程设置 alarm(PROBE_TIMEOUT_SEC=5s) 后执行 lstat
 *         - 父进程记录 active_probe_pid 和 active_probe_dev
 *         每次仅允许一个活跃探测进程，避免并发探测导致误判。
 */
static void dispatch_probes(Monitor *mon) {
    AppContext *ctx = mon->ctx;
    if (!ctx || !ctx->probe_scheduler) return;
    if (mon->active_probe_pid > 0) return;

    ProbeTask task;
    if (!probe_scheduler_peek(ctx->probe_scheduler, &task)) return;
    if (task.s_status == SP_STATUS_CONDEMNED) {
        probe_scheduler_remove_dev(ctx->probe_scheduler, task.dev);
        return;
    }

    probe_scheduler_remove_dev(ctx->probe_scheduler, task.dev);

    pid_t pid = fork();
    if (pid == 0) {
        alarm(PROBE_TIMEOUT_SEC);
        struct stat st;
        (void)lstat(task.probe_path, &st);
        _exit(0);
    } else if (pid > 0) {
        mon->active_probe_pid = pid;
        mon->active_probe_dev = task.dev;
    }
}

/**
 * @brief  收割已完成的敢死队探测进程
 * @param  mon  Monitor*  监控器指针，不能为空
 * @return void
 *
 * @note   以 WNOHANG 方式 waitpid 检查活跃探测进程：
 *         - 若正常退出（WIFEXITED）：设备恢复为 NORMAL，将 spbin 中该设备的积压路径重新入队扫描
 *         - 若异常退出（被 signal 杀死，通常是 alarm 超时）：重新推入 ProbeScheduler 进行指数退避重试
 *         无论结果如何，最后重置 active_probe_pid 为 -1，允许调度下一个探测任务。
 */
static void reap_probes(Monitor *mon) {
    if (mon->active_probe_pid <= 0) return;

    int status;
    pid_t rc = waitpid(mon->active_probe_pid, &status, WNOHANG);
    if (rc == 0) return;

    AppContext *ctx = mon->ctx;
    if (rc == mon->active_probe_pid) {
        if (WIFEXITED(status)) {
            dev_mgr_mark_alive(ctx->dev_mgr, mon->active_probe_dev);
            spbin_requeue_recovered(ctx, mon->active_probe_dev);
        } else {
            ProbeTask task = {0};
            task.dev = mon->active_probe_dev;
            task.probe_interval = PROBE_INTERVAL_INITIAL;
            task.next_probe_time = time(NULL) + PROBE_INTERVAL_INITIAL;
            task.retry_count = 0;
            task.s_status = SP_STATUS_PROBING;

            for (size_t i = 0; i < ctx->spbin_count; i++) {
                if (ctx->spbin_entries[i].dev == mon->active_probe_dev) {
                    safe_strcpy(task.probe_path, ctx->spbin_entries[i].path, sizeof(task.probe_path));
                    break;
                }
            }
            probe_scheduler_push(ctx->probe_scheduler, &task);
        }
    }

    mon->active_probe_pid = -1;
    mon->active_probe_dev = 0;
}

/* ================================================================
 * Monitor thread lifecycle
 * ================================================================ */

/**
 * @brief  监控线程主入口函数
 * @param  arg  void*  指向 Monitor 结构体的指针，不能为空
 * @return void*  始终返回 NULL
 *
 * @note   循环周期约为 MONITOR_INTERVAL_MS（500ms）。每轮循环：
 *         1. 若未开启 mute，打印监控面板到 stdout
 *         2. 每 CHECK_INTERVAL_SEC（1s）执行一次健康检查和探测调度
 *         -M (--mute) 仅静默监控面板和诊断信息，不影响扫描数据输出
 *         3. 每轮都尝试收割探测进程（因为探测可能在任意时刻完成）
 *         4. usleep 500ms 后继续下一轮
 *         当 mon->running 被主线程设为 false 时退出循环。
 */
void* monitor_thread_entry(void *arg) {
    Monitor *mon = (Monitor*)arg;
    time_t last_check_time = 0;

    while (mon->running) {
        if (!mon->ctx->cfg.mute) {
            print_progress(mon);
        }

        time_t now = time(NULL);
        if (now - last_check_time >= CHECK_INTERVAL_SEC) {
            check_workers_health(mon);
            dispatch_probes(mon);
            last_check_time = now;
        }
        reap_probes(mon);

        usleep(MONITOR_INTERVAL_MS * 1000);
    }

    return NULL;
}

/**
 * @brief  创建 Monitor 实例
 * @param  ctx  AppContext*  应用上下文指针，不能为空
 * @return Monitor*  成功返回指向新分配监控器的指针；内存不足时返回 NULL
 *
 * @note   初始化 running 为 true，active_probe_pid 为 -1。
 *         监控线程本身由 main.c 创建（pthread_create），不在本函数内创建。
 */
Monitor* monitor_create(AppContext *ctx) {
    Monitor *mon = calloc(1, sizeof(Monitor));
    if (!mon) return NULL;

    mon->ctx = ctx;
    mon->running = true;
    mon->active_probe_pid = -1;

    return mon;
}

/**
 * @brief  销毁 Monitor 实例
 * @param  mon  Monitor*  要销毁的监控器指针，允许传入 NULL（空操作）
 * @return void
 *
 * @note   设置 running 为 false 并 pthread_join 等待监控线程结束，然后释放内存。
 *         若监控线程尚未创建（tid 未设置），join 行为未定义，
 *         因此调用方应确保线程已创建后再调用本函数。
 */
void monitor_destroy(Monitor *mon) {
    if (!mon) return;
    mon->running = false;
    pthread_join(mon->tid, NULL);
    free(mon);
}

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

static double calculate_rate_simple(time_t start_time, unsigned long count) {
    time_t current_time = time(NULL);
    double elapsed = difftime(current_time, start_time);
    if (elapsed < 1.0) return 0.0;
    return (double)count / elapsed;
}

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

void print_progress(Monitor *mon) {
    AppContext *ctx = mon->ctx;
    if (!ctx) return;

    RuntimeState *state = &ctx->state;
    Config *cfg = &ctx->cfg;

    update_statistics(state);

    FILE *fp = stderr;

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

static void check_workers_health(Monitor *mon) {
    AppContext *ctx = mon->ctx;
    if (!ctx || !ctx->worker_pool) return;

    time_t now = time(NULL);
    int timeout = ctx->cfg.heartbeat_timeout;

    for (int i = 0; i < ctx->worker_pool->num_workers; i++) {
        WorkerSlot *slot = &ctx->worker_pool->slots[i];
        if (!slot->is_alive) continue;

        if (now - slot->last_heartbeat > timeout) {
            fprintf(stderr, "[Monitor] Worker %d heartbeat timeout (dev=%lu, path=%s). Replacing.\n",
                    i, (unsigned long)slot->current_dev, slot->current_path);

            kill(slot->pid, SIGKILL);
            int status;
            waitpid(slot->pid, &status, WNOHANG);

            slot->is_alive = false;
            slot->pid = -1;
            ctx->worker_pool->active_count--;
            ctx->state.has_error = true;
        }
    }
}

/* ================================================================
 * Daredevil probe (fork-based, same logic as old main_loop.c)
 * ================================================================ */

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

Monitor* monitor_create(AppContext *ctx) {
    Monitor *mon = calloc(1, sizeof(Monitor));
    if (!mon) return NULL;

    mon->ctx = ctx;
    mon->running = true;
    mon->active_probe_pid = -1;

    return mon;
}

void monitor_destroy(Monitor *mon) {
    if (!mon) return;
    mon->running = false;
    pthread_join(mon->tid, NULL);
    free(mon);
}

#define _GNU_SOURCE
#include "main_loop.h"
#include "utils.h"
#include "progress.h"
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

#define EVENTFD_SLOT_ID 0xFFFFFFFFU

/* ================================================================
 * Batch payload parser
 * ================================================================ */
typedef struct {
    char **paths;
    struct stat *stats;
    int count;
} ParsedBatch;

static void parsed_batch_free(ParsedBatch *b) {
    if (!b) return;
    for (int i = 0; i < b->count; i++) free(b->paths[i]);
    free(b->paths);
    free(b->stats);
    b->paths = NULL;
    b->stats = NULL;
    b->count = 0;
}

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
                ipc_send(ctx->worker_pool->slots[batch->worker_id].fd_in, IPC_MSG_SCAN, path, plen);
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
            if (ctx->cfg.print_dir && ctx->state.dir_info_fp) {
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
    ctx->state.total_dequeued_count++;
    
    /* 释放 batch 内存 */
    for (int i = 0; i < batch->count; i++) free(batch->paths[i]);
    free(batch->paths);
    free(batch->stats);
    free(batch->results);
    free(batch);
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

void main_loop_run(AppContext *ctx) {
    ctx->epfd = epoll_create1(EPOLL_CLOEXEC);
    if (ctx->epfd < 0) {
        perror("epoll_create1");
        return;
    }

    struct epoll_event ev;
    for (int i = 0; i < ctx->worker_pool->num_workers; i++) {
        memset(&ev, 0, sizeof(ev));
        ev.events = EPOLLIN;
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

            IpcMessageHeader hdr;
            if (ipc_recv_header(ctx->worker_pool->slots[slot_id].fd_out, &hdr) != 0) {
                /* Worker pipe broken, mark dead */
                ctx->worker_pool->slots[slot_id].is_alive = false;
                ctx->worker_pool->active_count--;
                continue;
            }

            if (hdr.payload_len > 16 * 1024 * 1024) {
                /* Sanity check: refuse huge payload */
                void *tmp = malloc(hdr.payload_len);
                if (tmp) { ipc_recv_payload(ctx->worker_pool->slots[slot_id].fd_out, tmp, hdr.payload_len); free(tmp); }
                continue;
            }

            void *payload = NULL;
            if (hdr.payload_len > 0) {
                payload = malloc(hdr.payload_len);
                if (ipc_recv_payload(ctx->worker_pool->slots[slot_id].fd_out, payload, hdr.payload_len) != 0) {
                    free(payload);
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

        /* Monitor routines */
        monitor_check_timeouts(ctx);
        monitor_dispatch_probes(ctx);
        monitor_reap_probes(ctx);

        /* Pump historical pbin directories during recovery */
        if (ctx->hist_pump_state == HIST_PUMP_OLD || ctx->hist_pump_state == HIST_PUMP_NEW) {
            pump_pbin_batch(ctx, ctx->cfg.batch_size);
        }

        /* Replace dead workers */
        for (int i = 0; i < ctx->worker_pool->num_workers; i++) {
            WorkerSlot *slot = &ctx->worker_pool->slots[i];
            if (!slot->is_alive && slot->pid == -1) {
                /* pid == -1 means killed by monitor but not yet reaped/replaced */
                epoll_ctl(ctx->epfd, EPOLL_CTL_DEL, slot->fd_out, NULL);
                close(slot->fd_in);
                close(slot->fd_out);
                worker_pool_replace(ctx->worker_pool, i);
                /* Re-add new fd_out to epoll */
                memset(&ev, 0, sizeof(ev));
                ev.events = EPOLLIN;
                ev.data.u32 = (uint32_t)i;
                epoll_ctl(ctx->epfd, EPOLL_CTL_ADD, slot->fd_out, &ev);
            }
        }

        /* Termination condition */
        if (atomic_load(&ctx->pending_tasks) == 0 && !ctx->resume_active) {
            bool all_idle = true;
            for (int i = 0; i < ctx->worker_pool->num_workers; i++) {
                if (ctx->worker_pool->slots[i].is_alive) {
                    /* TODO: check if worker is actually idle */
                }
            }
            if (all_idle) {
                /* Graceful stop */
                worker_pool_stop_all(ctx->worker_pool);
                ctx->running = false;
            }
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
    if (thread_pool_submit(ctx->thread_pool, batch)) {
        /* 提交成功，ParsedBatch 内存所有权转移给 batch */
        return;
    }

    /* 队列满，降级为同步处理 */
    batch_dedup_worker(batch, ctx);
    process_completed_batch(ctx, batch);
}

void main_loop_handle_heartbeat(AppContext *ctx, int worker_id, uint64_t timestamp) {
    if (worker_id < 0 || worker_id >= ctx->worker_pool->num_workers) return;
    ctx->worker_pool->slots[worker_id].last_heartbeat = (time_t)timestamp;
}

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

void main_loop_handle_exit(AppContext *ctx, int worker_id) {
    if (worker_id < 0 || worker_id >= ctx->worker_pool->num_workers) return;
    WorkerSlot *slot = &ctx->worker_pool->slots[worker_id];
    slot->is_alive = false;
    ctx->worker_pool->active_count--;
    /* Reap zombie immediately */
    int status;
    waitpid(slot->pid, &status, WNOHANG);
}

/* ================================================================
 * Monitor routines
 * ================================================================ */

static pid_t g_probe_pid = -1;
static dev_t g_probe_dev = 0;

void monitor_check_timeouts(AppContext *ctx) {
    time_t now = time(NULL);
    for (int i = 0; i < ctx->worker_pool->num_workers; i++) {
        WorkerSlot *slot = &ctx->worker_pool->slots[i];
        if (!slot->is_alive) continue;
        if (now - slot->last_heartbeat > ctx->cfg.heartbeat_timeout) {
            fprintf(stderr, "[Monitor] Worker %d heartbeat timeout. Replacing.\n", i);
            kill(slot->pid, SIGKILL);
            int status;
            waitpid(slot->pid, &status, WNOHANG);
            slot->is_alive = false;
            slot->pid = -1; /* mark as needs replacement */
            ctx->worker_pool->active_count--;
        }
    }
}

void monitor_dispatch_probes(AppContext *ctx) {
    if (g_probe_pid > 0) return; /* already have an active probe */

    ProbeTask task;
    if (!probe_scheduler_peek(ctx->probe_scheduler, &task)) return;
    if (task.s_status == SP_STATUS_CONDEMNED) {
        probe_scheduler_remove_dev(ctx->probe_scheduler, task.dev);
        return;
    }

    /* Pop the due task */
    probe_scheduler_remove_dev(ctx->probe_scheduler, task.dev);

    pid_t pid = fork();
    if (pid == 0) {
        /* Child: daredevil probe */
        alarm(PROBE_TIMEOUT_SEC);
        struct stat st;
        int rc = lstat(task.probe_path, &st);
        (void)rc;
        _exit(0); /* return = alive */
    } else if (pid > 0) {
        g_probe_pid = pid;
        g_probe_dev = task.dev;
    }
}

void monitor_reap_probes(AppContext *ctx) {
    if (g_probe_pid <= 0) return;

    int status;
    pid_t rc = waitpid(g_probe_pid, &status, WNOHANG);
    if (rc == 0) return; /* still running */

    if (rc == g_probe_pid) {
        if (WIFEXITED(status)) {
            /* Probe returned: device is alive */
            dev_mgr_mark_alive(ctx->dev_mgr, g_probe_dev);
            spbin_requeue_recovered(ctx, g_probe_dev);
        } else {
            /* Killed or crashed: device still dead, reschedule */
            ProbeTask task = {0};
            task.dev = g_probe_dev;
            task.probe_interval = PROBE_INTERVAL_INITIAL; /* will be doubled by scheduler */
            task.next_probe_time = time(NULL) + PROBE_INTERVAL_INITIAL;
            task.retry_count = 0;
            task.s_status = SP_STATUS_PROBING;
            /* probe_path: find from spbin_entries */
            for (size_t i = 0; i < ctx->spbin_count; i++) {
                if (ctx->spbin_entries[i].dev == g_probe_dev) {
                    safe_strcpy(task.probe_path, ctx->spbin_entries[i].path, sizeof(task.probe_path));
                    break;
                }
            }
            probe_scheduler_push(ctx->probe_scheduler, &task);
        }
    }

    g_probe_pid = -1;
    g_probe_dev = 0;
}

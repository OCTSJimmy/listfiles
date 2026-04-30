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
#include <sys/wait.h>
#include <signal.h>
#include <stdatomic.h>

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

    struct epoll_event events[64];
    ctx->running = true;

    while (ctx->running) {
        int nfds = epoll_wait(ctx->epfd, events, 64, 500); /* 500ms timeout */

        for (int i = 0; i < nfds; i++) {
            uint32_t slot_id = events[i].data.u32;
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

        /* Monitor routines */
        monitor_check_timeouts(ctx);
        monitor_dispatch_probes(ctx);
        monitor_reap_probes(ctx);

        /* Replace dead workers */
        for (int i = 0; i < ctx->worker_pool->num_workers; i++) {
            if (!ctx->worker_pool->slots[i].is_alive && ctx->worker_pool->slots[i].pid != 0) {
                /* epoll_ctl DEL before close (fd must be valid for DEL) */
                epoll_ctl(ctx->epfd, EPOLL_CTL_DEL, ctx->worker_pool->slots[i].fd_out, NULL);
                close(ctx->worker_pool->slots[i].fd_in);
                close(ctx->worker_pool->slots[i].fd_out);
                worker_pool_replace(ctx->worker_pool, i);
                /* Re-add new fd_out to epoll */
                memset(&ev, 0, sizeof(ev));
                ev.events = EPOLLIN;
                ev.data.u32 = (uint32_t)i;
                epoll_ctl(ctx->epfd, EPOLL_CTL_ADD, ctx->worker_pool->slots[i].fd_out, &ev);
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

    close(ctx->epfd);
    ctx->epfd = -1;
}

/* ================================================================
 * Message handlers
 * ================================================================ */

void main_loop_handle_batch(AppContext *ctx, int worker_id, const void *payload, uint32_t len) {
    (void)worker_id;
    ParsedBatch batch;
    if (!parse_batch(payload, len, &batch)) return;

    for (int i = 0; i < batch.count; i++) {
        const char *path = batch.paths[i];
        struct stat *st = &batch.stats[i];

        /* Compute fingerprint and check visited set */
        uint8_t fp[FP_SIZE];
        fp_compute(path, st->st_dev, st->st_ino, fp);
        if (fp_set_insert(ctx->visited_set, fp)) {
            continue; /* already visited */
        }

        /* Device blacklist check */
        if (dev_mgr_is_blacklisted(ctx->dev_mgr, st->st_dev)) {
            ctx->state.has_error = true;
            continue;
        }

        if (S_ISDIR(st->st_mode)) {
            /* Dispatch sub-directory to a worker */
            atomic_fetch_add(&ctx->pending_tasks, 1);
            uint32_t plen = (uint32_t)strlen(path);
            ipc_send(ctx->worker_pool->slots[worker_id].fd_in, IPC_MSG_SCAN, path, plen);

            ctx->state.dir_count++;
            if (ctx->cfg.include_dir) {
                async_writer_submit(ctx->async_writer, path, st);
            }
            if (ctx->cfg.print_dir && ctx->state.dir_info_fp) {
                fprintf(ctx->state.dir_info_fp, "%s%s\n", OUTPUT_DIR_PREFIX, path);
            }
            if (ctx->cfg.continue_mode) {
                record_path(&ctx->cfg, &ctx->state, path, st);
            }
        } else {
            ctx->state.file_count++;
            async_writer_submit(ctx->async_writer, path, st);
            if (ctx->cfg.continue_mode) {
                record_path(&ctx->cfg, &ctx->state, path, st);
            }
        }
    }

    atomic_fetch_sub(&ctx->pending_tasks, 1);
    ctx->state.total_dequeued_count++;
    parsed_batch_free(&batch);
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



/**
 * @file batch_processor.c
 * @brief Batch 解析、CPU 去重、完成处理与批量排空
 *
 * 负责将从 Worker 接收的原始 IPC BATCH payload 解析为结构化数据，
 * 提交到 CPU 去重线程池，并在主线程中处理完成后的批次。
 */
#define _GNU_SOURCE
#include "main_loop.h"
#include "utils.h"
#include "progress.h"
#include "worker_proc.h"
#include "log.h"
#include "msg_format.h"
#include "msg_queue.h"
#include "ipc_thread.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include <stdatomic.h>

/* ================================================================
 * ParsedBatch helpers
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

    if (bh.count > 1000000) {
        log_error("[Batch] count %u exceeds sanity limit", bh.count);
        return false;
    }

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

void batch_dedup_worker(TPBatch *batch, void *user_data) {
    /* v15.1.4: defensive sanity check */
    if (!batch || batch->count < 0 || batch->count > 1000000) {
        log_fatal("[DedupWorker] batch invalid or count out of range: %p count=%d",
                  (void*)batch, batch ? batch->count : -999);
        return;
    }

    AppContext *ctx = user_data;
    const int ITERATION_LIMIT = 100000; /* v15.1.2: hard timeout for single batch */
    for (int i = 0; i < batch->count; i++) {
        if (i >= ITERATION_LIMIT) {
            log_fatal("[DedupWorker] batch iteration exceeded limit %d (count=%d). Aborting to prevent CPU spin.",
                      ITERATION_LIMIT, batch->count);
            for (int j = i; j < batch->count; j++) {
                batch->results[j] = 1; /* duplicate (skip) */
            }
            return;
        }
        const char *path = batch->paths[i];
        struct stat *st = &batch->stats[i];
        uint8_t fp[FP_SIZE];
        fp_compute(path, st->st_dev, st->st_ino, fp);
        uint8_t result = 0;
        /* v15.4.4: In HIST_PUMP_OLD phase, skip visited_set dedup for directories
         * so that re-scanning can discover sub-directories that were lost during
         * the previous interrupted run. Files are still deduped to avoid duplicate
         * output entries. */
        bool is_dir = S_ISDIR(st->st_mode);
        if (!is_dir || ctx->hist_pump_state != HIST_PUMP_OLD) {
            if (fp_set_insert(ctx->visited_set, fp)) {
                result |= 1; /* duplicate */
            }
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
    /* v15.1.4: defensive sanity check to prevent CPU spin from corrupted count */
    if (!batch || batch->count < 0 || batch->count > 1000000) {
        log_fatal("[Batch] batch invalid or count out of range: %p count=%d, worker=%d. Dropping.",
                  (void*)batch, batch ? batch->count : -999,
                  batch ? batch->worker_id : -999);
        if (batch) {
            for (int i = 0; i < batch->count && i < 1000000; i++) free(batch->paths[i]);
            free(batch->paths);
            free(batch->stats);
            free(batch->results);
            free(batch);
        }
        atomic_fetch_sub(&ctx->pending_batches, 1);
        return;
    }

    log_debug_v(202605181600UL, "[Batch] process_completed_batch start worker=%d count=%d pending_batches=%ld",
              batch->worker_id, batch->count, atomic_load(&ctx->pending_batches));
    OutputBatch out_batch = {0};

    const int PROC_ITER_LIMIT = 1000000; /* v15.1.4 */
    for (int i = 0; i < batch->count; i++) {
        if (i >= PROC_ITER_LIMIT) {
            log_fatal("[Batch] process_completed_batch iteration limit exceeded: worker=%d count=%d i=%d. Aborting batch.",
                      batch->worker_id, batch->count, i);
            break;
        }
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
                /* v15.4.4: During resume pumping, re-scan discovered directories
                 * to recover sub-directories lost in the previous interrupted run. */
            }
            /* Unified dispatch: send CMD_SCAN for all non-duplicate directories.
             * In HIST_PUMP_OLD, batch_dedup_worker no longer skips directories,
             * so they reach here and get re-dispatched. */
            atomic_fetch_add(&ctx->pending_tasks, 1);
            int wid = dispatch_find_idle_worker(ctx);
            if (wid < 0) {
                log_warn("[Dispatch] no IDLE worker available (path=%s), requeue to lost_tasks", path_log_mask(path));
                atomic_fetch_sub(&ctx->pending_tasks, 1);
                lost_tasks_push(&ctx->lost_tasks, strdup(path));
                continue;
            }
            WorkerSlot *slot = &ctx->worker_pool->slots[wid];
            atomic_store(&slot->state, WORKER_STATE_BUSY);
            slot->current_dev = st->st_dev;
            safe_strcpy(slot->current_path, path, sizeof(slot->current_path));
            if (!send_scan_to_ipc(ctx, wid, path, st->st_dev)) {
                atomic_fetch_sub(&ctx->pending_tasks, 1);
                atomic_store(&slot->state, WORKER_STATE_IDLE);
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

    atomic_fetch_sub(&ctx->pending_batches, 1);
    log_debug_v(202605181600UL, "[Batch] pending_batches after sub: %ld", atomic_load(&ctx->pending_batches));
    ctx->state.total_dequeued_count++;

    for (int i = 0; i < batch->count; i++) free(batch->paths[i]);
    free(batch->paths);
    free(batch->stats);
    free(batch->results);
    free(batch);
}

void drain_completed_batches(AppContext *ctx) {
    TPBatch *batch;
    while ((batch = thread_pool_poll_completed(ctx->thread_pool)) != NULL) {
        process_completed_batch(ctx, batch);
    }
}

/* ================================================================
 * Public batch handler (called from main_loop message router)
 * ================================================================ */

void main_loop_handle_batch(AppContext *ctx, int worker_id, const void *payload, uint32_t len) {
    ParsedBatch parsed;
    if (!parse_batch(payload, len, &parsed)) {
        log_error("[Batch] Worker %d parse_batch FAILED (len=%u)", worker_id, len);
        return;
    }
    log_debug_v(202605181600UL, "[Batch] Worker %d parse_batch OK (count=%d)", worker_id, parsed.count);

    /* v15.1.4: defensive sanity check on parsed.count */
    if (parsed.count < 0 || parsed.count > 1000000) {
        log_fatal("[Batch] parsed.count out of range: %d (worker=%d, len=%u), dropping batch",
                  parsed.count, worker_id, len);
        parsed_batch_free(&parsed);
        return;
    }

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

    log_debug_v(202605181600UL, "[Batch] pending_batches before add: %ld", atomic_load(&ctx->pending_batches));
    atomic_fetch_add(&ctx->pending_batches, 1);
    log_debug_v(202605181600UL, "[Batch] pending_batches after add: %ld", atomic_load(&ctx->pending_batches));
    if (thread_pool_submit(ctx->thread_pool, batch)) {
        log_debug_v(202605181600UL, "[Batch] Worker %d submitted to thread pool", worker_id);
        return;
    }

    log_debug_v(202605181600UL, "[Batch] Worker %d thread pool full, inline processing", worker_id);
    batch_dedup_worker(batch, ctx);
    process_completed_batch(ctx, batch);
}

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
#include "circuit_breaker.h"
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
        uint8_t result = 0;
        /* v15.6.0（P0-004 统一队列模型）：discovered_set 仅含目录——所有阶段
         * （含 HIST_PUMP_OLD）均对目录查重，防环/防重复发现。v15.4.5 的 OLD 阶段
         * 跳过特例已删除：统一队列模型下泵送差集覆盖所有"已发现未完成"目录，
         * 无需靠跳过去重补救丢失的子目录。
         * 文件不进入目录任务去重集合——输出语义 at-least-once 允许重复行。 */
        bool is_dir = S_ISDIR(st->st_mode);
        if (is_dir) {
            uint8_t fp[FP_SIZE];
            fp_compute(path, st->st_dev, st->st_ino, fp);
            if (fp_set_insert(ctx->discovered_set, fp)) {
                result |= 1; /* duplicate */
            }
        }
        /* v15.5.6: 单挂载保护——与根路径同设备时禁用设备级跳过，
         * 防止 NFS 单挂载点因个别目录超时被整体熔断。 */
        if (st->st_dev != ctx->state.root_dev) {
            if (dev_mgr_is_blacklisted(ctx->dev_mgr, st->st_dev)) {
                result |= 2; /* blacklisted */
            }
        }
        batch->results[i] = result;
    }
}

/* ================================================================
 * Side effects for a completed batch (must run on main thread)
 * ================================================================ */

/* v15.6.0（P0-002 输出三态）：提交输出 batch 前登记 output_pending[slot]（OUTPUT_QUEUED），
 * 输出线程 fflush 后递减（OUTPUT_COMMITTED），目录完成屏障等待其归零才写 dpbin。
 * 条目语义：pbin 先记（DISCOVERED）→ 此处提交输出线程（OUTPUT_QUEUED）→
 * 输出线程 fflush + 计数回调（OUTPUT_COMMITTED）；"pbin 先于输出提交"的顺序不变。
 * 无有效 slot 归属时按 -1 提交、不计数。 */
static void submit_output_batch(AppContext *ctx, OutputBatch *out_batch, int slot_id) {
    if (slot_id >= 0 && ctx->worker_pool && slot_id < ctx->worker_pool->num_workers
        && ctx->output_pending) {
        out_batch->slot_id = slot_id;
        atomic_fetch_add(&ctx->output_pending[slot_id], 1);
    } else {
        out_batch->slot_id = -1;
    }
    async_writer_submit_batch(ctx->async_writer, out_batch);
}

static void process_completed_batch(AppContext *ctx, TPBatch *batch) {
    /* v15.1.4: defensive sanity check to prevent CPU spin from corrupted count */
    if (!batch || batch->count < 0 || batch->count > 1000000) {
        log_fatal("[Batch] batch invalid or count out of range: %p count=%d, worker=%d. Dropping.",
                  (void*)batch, batch ? batch->count : -999,
                  batch ? batch->worker_id : -999);
        if (batch) {
            /* v15.6.0: 批次处理完成计数（P0-001 屏障）——防御性丢弃路径同样闭环 */
            if (batch->worker_id >= 0 && ctx->worker_pool
                && batch->worker_id < ctx->worker_pool->num_workers) {
                atomic_fetch_add(&ctx->worker_pool->slots[batch->worker_id].batches_processed, 1);
            }
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
            circuit_breaker_record(ctx, "BLACKLIST", path, st->st_dev, 0);
            continue; /* blacklisted */
        }

        if (S_ISDIR(st->st_mode)) {
            if (ctx->hist_pump_state == HIST_PUMP_OLD) {
                fpbin_append(ctx, path, st);
                /* v15.4.5: During resume pumping, re-scan discovered directories
                 * to recover sub-directories lost in the previous interrupted run. */
            }
            /* v15.6.0（P0-004 统一队列模型）：新目录统一经 enqueue_dir 入队——
             * completed_set 差集剪枝、enqueued_set 防重复入队、
             * dispatch_queue（未达 HIGH_WATER）/dspill（超水位兜底）分流均在其内。
             * 无 progress_base 时无兜底通道，宁可队列膨胀也不丢目录。 */
            enqueue_dir(ctx, path, st);

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
            if (ctx->hist_pump_state != HIST_PUMP_OLD) {
                /* v15.6.0：pbin 全量记录（不再要求 continue_mode）——pbin 是
                 * 盲信扫描的基准来源（§8.1），基准则必须是任意完整全量运行。
                 * --clean 模式由 record_path 内部早退，不保留任何进度文件。
                 * HIST_PUMP_OLD 阶段新发现目录走 fpbin（P0-003），不写 pbin。 */
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
            /* v15.6.0：pbin 全量记录（含文件，盲信基准来源），不再要求 continue_mode */
            record_path_batch_append(&ctx->cfg, &ctx->state, &ctx->record_batch, path, st);
        }

        if (out_batch.count >= ASYNC_BATCH_SIZE) {
            submit_output_batch(ctx, &out_batch, batch->worker_id);
        }
    }

    if (out_batch.count > 0) {
        submit_output_batch(ctx, &out_batch, batch->worker_id);
    }

    atomic_fetch_sub(&ctx->pending_batches, 1);
    /* v15.6.0: 批次处理完成计数（P0-001 屏障），供 advance_task_barriers 判定 */
    if (batch->worker_id >= 0 && ctx->worker_pool
        && batch->worker_id < ctx->worker_pool->num_workers) {
        atomic_fetch_add(&ctx->worker_pool->slots[batch->worker_id].batches_processed, 1);
    }
    log_debug_v(202605181600UL, "[Batch] pending_batches after sub: %ld", atomic_load(&ctx->pending_batches));
    ctx->state.total_dequeued_count++;

    for (int i = 0; i < batch->count; i++) free(batch->paths[i]);
    free(batch->paths);
    free(batch->stats);
    free(batch->results);
    free(batch);
}

int drain_completed_batches(AppContext *ctx) {
    TPBatch *batch;
    int drained = 0;
    while ((batch = thread_pool_poll_completed(ctx->thread_pool)) != NULL) {
        process_completed_batch(ctx, batch);
        drained++;
    }
    return drained;
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

    /* v15.6.0: 批次接收计数（P0-001 屏障）——解析成功、确定会被处理才计数；
     * 解析失败/OOM 丢弃的批次不计 received，也不计 processed，屏障账目保持平衡。
     * 协议空批次（count==0，空目录的收尾批次）照常计数，自然闭环。
     * v15.6.0（P0-007）：错误路径不再尾随空 BATCH——Master 收到 RET_ERROR 即
     * 销账并清零该任务的屏障计数，滞留批次由 epoch 校验丢弃。 */
    if (worker_id >= 0 && ctx->worker_pool && worker_id < ctx->worker_pool->num_workers) {
        atomic_fetch_add(&ctx->worker_pool->slots[worker_id].batches_received, 1);
    }

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

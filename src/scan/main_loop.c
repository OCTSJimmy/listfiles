/**
 * @file main_loop.c
 * @brief 主消息总线与调度循环框架
 *
 * 负责：
 * - IPC 返回消息路由（handle_return_message）
 * - IPC 线程生命周期管理（init/destroy/stop）
 * - 主循环：cond_wait → drain ret_queue → drain batches → pump pbin → reap zombies → replace dead → dispatch lost → check termination
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
#include "dispatch_queue.h"
#include "circuit_breaker.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include <sys/wait.h>
#include <signal.h>
#include <stdatomic.h>
#include <dirent.h>
#include <sys/eventfd.h>

/* ================================================================
 * Handle return messages from IPC threads
 * ================================================================ */

/**
 * @brief  校验死亡类消息（RET_DEAD/RET_DEV_TIMEOUT/RET_EXIT）是否属于当前代
 * @param  ctx              AppContext*    应用上下文
 * @param  msg              IpcThreadMsg*  待校验消息
 * @param  reported_pid_out pid_t*         输出：消息携带的 worker pid，允许为 NULL
 * @return bool  true = 同代（受理）；false = 跨代残留或畸形（丢弃）
 *
 * @note   v15.6.1（P0-101）：此前 RET_DEAD 无载荷，stale 判定
 *         （is_alive && pid != -1 → 丢弃）恰好把每一条真实死亡都误判为残留——
 *         Worker 崩溃永不清理、永不替换、永不销账（生产事故 R1）。
 *         现在死亡类消息携带 IPC 线程观测到的 pid，仅当 reported_pid == slot->pid
 *         时受理；换代后 slot->pid 变更，旧代消息自然失配被丢弃。
 */
static bool death_msg_current_generation(AppContext *ctx, IpcThreadMsg *msg, pid_t *reported_pid_out) {
    int sid = msg->slot_id;
    if (!ctx->worker_pool || sid < 0 || sid >= ctx->worker_pool->num_workers) return false;
    pid_t reported = -1;
    if (msg->type == RET_DEV_TIMEOUT || msg->type == RET_ERROR || msg->type == RET_ENTRY_ERROR) {
        if (!msg->data || msg->data_len < sizeof(RetErrorPayload)) {
            log_error("[Bus] RET type=%u missing/short payload (slot=%d), dropped", msg->type, sid);
            return false;
        }
        reported = ((RetErrorPayload*)msg->data)->reported_pid;
    } else {
        if (!msg->data || msg->data_len < sizeof(RetDeathPayload)) {
            log_error("[Bus] death message type=%u missing/short payload (slot=%d), dropped",
                      msg->type, sid);
            return false;
        }
        reported = ((RetDeathPayload*)msg->data)->reported_pid;
    }
    if (reported_pid_out) *reported_pid_out = reported;
    WorkerSlot *slot = &ctx->worker_pool->slots[sid];
    if (reported != slot->pid) {
        log_debug_v(202608241500UL, "[Bus] stale death message dropped: type=%u slot=%d reported_pid=%d current_pid=%d",
                    msg->type, sid, (int)reported, (int)slot->pid);
        return false;
    }
    return true;
}

static void handle_return_message(AppContext *ctx, IpcThreadMsg *msg) {
    log_debug("[Bus] received type=%u slot=%d len=%zu", msg->type, msg->slot_id, msg->data_len);

    /* v15.6.0: RET_BATCH/RET_FINISH 校验 epoch，丢弃旧 Worker 残留数据 */
    if (msg->type == RET_BATCH || msg->type == RET_FINISH) {
        int sid = msg->slot_id;
        if (sid < 0 || sid >= ctx->worker_pool->num_workers) {
            log_warn("丢弃过期 epoch 消息: slot=%d epoch=%lu current=(invalid slot)",
                     sid, (unsigned long)msg->epoch);
            free(msg->data);
            msg->data = NULL;
            return;
        }
        uint64_t current = ctx->worker_pool->slots[sid].current_epoch;
        if (msg->epoch != current) {
            log_warn("丢弃过期 epoch 消息: slot=%d epoch=%lu current=%lu",
                     sid, (unsigned long)msg->epoch, (unsigned long)current);
            free(msg->data);
            msg->data = NULL;
            return;
        }
    }

    switch (msg->type) {
        case RET_BATCH: {
            log_debug_v(202605150000UL, "[Bus] Worker %d BATCH (len=%zu)", msg->slot_id, msg->data_len);
            main_loop_handle_batch(ctx, msg->slot_id, msg->data, msg->data_len);
            break;
        }
        case RET_HEARTBEAT: {
            if (msg->data_len >= sizeof(RetHeartbeatPayload)) {
                RetHeartbeatPayload *hb = (RetHeartbeatPayload*)msg->data;
                main_loop_handle_heartbeat(ctx, msg->slot_id, hb->timestamp);
            }
            break;
        }
        case RET_ERROR: {
            /* v15.6.1（P0-101 补）：RET_ERROR 改变账目（pending--），必须同代校验——
             * 旧代 Worker 的迟到 ERROR 会对新一代 slot 误销账（混沌压测实测可致负） */
            if (!death_msg_current_generation(ctx, msg, NULL)) break;
            if (msg->data_len >= sizeof(RetErrorPayload)) {
                RetErrorPayload *err = (RetErrorPayload*)msg->data;
                IpcErrorHeader hdr = { err->errno_code, err->dev };
                /* v15.6.0（P0-007 RET_ERROR 状态机补全）：Worker 协议已改为错误路径
                 * 只发 IPC_MSG_ERROR（不再尾随空 BATCH/FINISH），Master 在此一次性完成
                 * 销账（pending_tasks--）、Worker 置 IDLE、写 spbin、设备探测调度。
                 * Worker 立即可被再派发（新 epoch）；旧任务的滞留 BATCH 会被 epoch
                 * 校验丢弃——这是正确行为（目录将整体重扫），无需特殊处理。 */
                main_loop_handle_error(ctx, msg->slot_id, &hdr, err->path);
            }
            break;
        }
        case RET_ENTRY_ERROR: {
            /* v15.5.7: 条目级错误（单条目 stat 失败、路径截断）——Worker 仍在正常扫描，
             * 不触发设备惩罚/探测/Worker 状态变更，仅记入熔断清单并累加 skipped_count，
             * 扫描结束时以非零退出码暴露不完整。
             * v15.6.1（P0-101 补）：同代校验——旧代迟到条目错误不应计入本轮熔断清单 */
            if (!death_msg_current_generation(ctx, msg, NULL)) break;
            if (msg->data_len >= sizeof(RetErrorPayload)) {
                RetErrorPayload *err = (RetErrorPayload*)msg->data;
                char reason[64];
                /* v15.5.8: errno_code==0 为 nlink oracle 失配（无 errno 的假空/假 EOF 旁证） */
                if (err->errno_code == 0)
                    snprintf(reason, sizeof(reason), "NLINK_MISMATCH");
                else
                    snprintf(reason, sizeof(reason), "ENTRY_ERROR(errno=%u)", err->errno_code);
                circuit_breaker_record(ctx, reason, err->path, (dev_t)err->dev, 0);
            }
            break;
        }
        case RET_READY: {
            log_info("[Bus] Worker %d READY", msg->slot_id);
            atomic_store(&ctx->worker_pool->slots[msg->slot_id].last_heartbeat, time(NULL));
            /* v15.1.1: 无论之前是 INITIALIZING 还是其他状态，收到 READY 后置 IDLE */
            atomic_store(&ctx->worker_pool->slots[msg->slot_id].state, WORKER_STATE_IDLE);
            break;
        }
        case RET_FINISH: {
            log_info_v(202605150000UL, "[Bus] Worker %d FINISH (pending_tasks=%ld)", msg->slot_id, atomic_load(&ctx->pending_tasks));
            /* v15.6.0（P0-001 完成屏障）：FINISH 仅推进到 DT_BATCHES_RECEIVED——
             * 不再立即 pending_tasks--、不再置 IDLE。待本任务全部 BATCH 被线程池
             * 处理完且输出全部 COMMITTED 后，由 advance_task_barriers 统一完结
             * （dpbin_append、pending_tasks--、Worker 置 IDLE 可被再次派发）。 */
            ctx->worker_pool->slots[msg->slot_id].task_state = DT_BATCHES_RECEIVED;
            break;
        }
        case RET_DEAD: {
            /* v15.6.1（P0-101）：同代校验——修复 stale 判定反转（真实死亡曾被
             * 100% 误吞：永不清理/替换/销账，生产事故 R1） */
            pid_t rpid;
            if (!death_msg_current_generation(ctx, msg, &rpid)) break;
            log_error("[Bus] Worker %d DEAD reported by IPC thread (pid=%d)", msg->slot_id, (int)rpid);
            cleanup_dead_worker_slot(ctx, msg->slot_id, true);
            break;
        }
        case RET_DEV_TIMEOUT: {
            /* v15.6.1（P0-106）：解除版本门控——scanner 卡死意味着该目录元数据
             * 可能丢失，按既定规则必须全局可见 */
            pid_t rpid;
            if (!death_msg_current_generation(ctx, msg, &rpid)) break;
            log_error("[Bus] Worker %d DEV_TIMEOUT (scanner stuck, pid=%d), replacing",
                      msg->slot_id, (int)rpid);
            cleanup_dead_worker_slot(ctx, msg->slot_id, true);
            break;
        }
        case RET_EXIT: {
            /* v15.6.1（P0-101/P0-109）：同代校验；携带在途任务的 EXIT = 非预期死亡。
             * 原实现 cleanup(redispatch_current=false)：在途目录不重入队、不写 spbin，
             * enqueued_set 又阻断重新发现 → 整棵子树静默丢失（生产事故 R9，
             * 8 亿 vs 1.9 亿缺口的真凶）。"正常退出"的合法时机仅 STOP 之后。 */
            pid_t rpid;
            if (!death_msg_current_generation(ctx, msg, &rpid)) break;
            WorkerSlot *slot = &ctx->worker_pool->slots[msg->slot_id];
            bool in_flight = (slot->task_state == DT_SCANNING
                           || slot->task_state == DT_BATCHES_RECEIVED
                           || slot->task_state == DT_BATCHES_PROCESSED);
            if (in_flight) {
                log_error("[Bus] Worker %d EXIT with in-flight task (pid=%d, task_state=%d), "
                          "按非预期死亡处理: %s",
                          msg->slot_id, (int)rpid, slot->task_state,
                          path_log_mask(slot->current_path));
                cleanup_dead_worker_slot(ctx, msg->slot_id, true);
            } else {
                log_warn("[Bus] Worker %d exit (pid=%d, 无在途任务)", msg->slot_id, (int)rpid);
                cleanup_dead_worker_slot(ctx, msg->slot_id, false);
            }
            break;
        }
        default: {
            log_error("[Bus] Worker %d UNKNOWN message type=%u (len=%zu)",
                     msg->slot_id, msg->type, msg->data_len);
            break;
        }
    }
    free(msg->data);
    msg->data = NULL;
}

/* ================================================================
 * Message handlers (mostly unchanged, accept payload directly)
 * ================================================================ */

void main_loop_handle_heartbeat(AppContext *ctx, int worker_id, uint64_t timestamp) {
    if (worker_id < 0 || worker_id >= ctx->worker_pool->num_workers) return;
    atomic_store(&ctx->worker_pool->slots[worker_id].last_heartbeat, (time_t)timestamp);
}

/**
 * @brief  RET_ERROR 完整处理（v15.6.0，P0-007 状态机补全 + P0-005 spbin 落盘）
 * @param  ctx        AppContext*          应用上下文指针，不能为空
 * @param  worker_id  int                  上报错误的 Worker slot 编号
 * @param  err        const IpcErrorHeader* 错误码与设备号，不能为空
 * @param  path       const char*          出错的目录路径，不能为空
 * @return void
 *
 * @note   状态机流转：BUSY --RET_ERROR--> IDLE，目录任务语义 = DEVICE_WAITING
 *         （在 spbin 中等待设备恢复重入队或跨会话恢复，不写 dpbin、不重入队）。
 *         处理顺序（先落 spbin 后销账，崩溃不一致时宁可 spbin 多记）：
 *         1. reason 分类（P1-002 errno 分类矩阵）：EACCES/EPERM → PERMISSION(4)；
 *            ETIMEDOUT → TIMEOUT(2)；EIO/ENODEV/ESTALE → PROBE_FAIL(1)；
 *            未知 errno → PROBE_FAIL 保守处理 + log_warn；
 *         2. 记熔断清单（沿用原语义：设备级记 DEV_TIMEOUT/EIO，其余记 DIR_ERROR）；
 *         3. 写 spbin（内存 + spbin_set + 磁盘 append-only，见 spbin_write_record）；
 *         4. 设备级错误（TIMEOUT/PROBE_FAIL 类）→ dev_mgr_mark_probing +
 *            push probe_task（沿用 probe_scheduler 敢死队探测状态机；
 *            ETIMEDOUT 原来 mark_dead，现按设计改 probing 语义）；
 *            PERMISSION 类不做设备探测；
 *         5. 放弃该任务的完成屏障（task_state=DT_NONE，batches 计数清零，
 *            不写 dpbin——目录未完成，留给 spbin/Reset 援救），
 *            pending_tasks--（Worker 已释放任务），Worker 置 IDLE 可被再派发。
 *         旧任务的滞留 BATCH 会被 epoch 校验丢弃——正确行为，目录将整体重扫。
 */
void main_loop_handle_error(AppContext *ctx, int worker_id, const IpcErrorHeader *err, const char *path) {
    dev_t dev = (dev_t)err->dev;

    /* 1. reason 分类 */
    uint8_t reason;
    bool device_level;
    switch ((int)err->errno_code) {
        case EACCES:
        case EPERM:
            reason = SP_REASON_PERMISSION;
            device_level = false;
            break;
        case ETIMEDOUT:
            reason = SP_REASON_TIMEOUT;
            device_level = true;
            break;
        case EIO:
        case ENODEV:
        case ESTALE:
            reason = SP_REASON_PROBE_FAIL;
            device_level = true;
            break;
        default:
            log_warn("[Error] Worker %d unknown errno=%u on %s, 按 PROBE_FAIL 保守处理",
                     worker_id, err->errno_code, path_log_mask(path));
            reason = SP_REASON_PROBE_FAIL;
            device_level = true;
            break;
    }

    log_error("[Monitor] Worker %d error on dev %lu: %s (errno=%u, reason=%u)",
              worker_id, (unsigned long)dev, path_log_mask(path), err->errno_code, reason);

    /* 2. 熔断清单 */
    if (device_level) {
        const char *cb_reason = (err->errno_code == ETIMEDOUT) ? "DEV_TIMEOUT" : "EIO";
        circuit_breaker_record(ctx, cb_reason, path, dev, 0);
        ctx->state.has_error = true;
    } else {
        /* v15.5.7: 目录级错误（如 EACCES 权限拒绝）不触发设备惩罚与探测 */
        char cb_reason[64];
        snprintf(cb_reason, sizeof(cb_reason), "DIR_ERROR(errno=%u)", err->errno_code);
        circuit_breaker_record(ctx, cb_reason, path, dev, 0);
    }

    /* 3. 写 spbin（先落盘后销账——崩溃不一致时宁可 spbin 多记，恢复时多扫不漏扫） */
    spbin_write_record(ctx, path, reason, dev);

    /* 4. 设备级错误 → PROBING + 敢死队探测（设备已在探测/判死则复用现有任务） */
    if (device_level && ctx->dev_mgr && ctx->probe_scheduler) {
        if (dev_mgr_get_state(ctx->dev_mgr, dev) == DEV_STATE_NORMAL) {
            dev_mgr_mark_probing(ctx->dev_mgr, dev);

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

    /* 5. 放弃完成屏障 + 销账 + Worker 置 IDLE（目录不重入队，避免重试风暴） */
    if (worker_id >= 0 && ctx->worker_pool && worker_id < ctx->worker_pool->num_workers) {
        WorkerSlot *slot = &ctx->worker_pool->slots[worker_id];
        slot->task_state = DT_NONE;
        atomic_store(&slot->batches_received, 0);
        atomic_store(&slot->batches_processed, 0);
        atomic_store(&slot->state, WORKER_STATE_IDLE);
    }
    atomic_fetch_sub(&ctx->pending_tasks, 1);
}

void main_loop_handle_exit(AppContext *ctx, int worker_id) {
    if (worker_id < 0 || worker_id >= ctx->worker_pool->num_workers) return;
    WorkerSlot *slot = &ctx->worker_pool->slots[worker_id];
    log_info("[Exit] Worker %d normal exit (pid=%d). active=%d->%d",
            worker_id, slot->pid,
            atomic_load(&ctx->worker_pool->active_count),
            atomic_load(&ctx->worker_pool->active_count) - 1);
    int status;
    waitpid(slot->pid, &status, WNOHANG);
    cleanup_dead_worker_slot(ctx, worker_id, false);
}

/* ================================================================
 * IPC Thread lifecycle helpers
 * ================================================================ */

bool init_ipc_threads(AppContext *ctx) {
    int n = ctx->worker_pool->num_workers;

    ctx->ipc_cmd_queues = calloc(n, sizeof(MsgQueue*));
    ctx->ipc_ret_queues = calloc(n, sizeof(MsgQueue*));
    ctx->ipc_threads = calloc(n, sizeof(IpcThreadCtx*));
    ctx->ipc_tids = calloc(n, sizeof(pthread_t));
    if (!ctx->ipc_cmd_queues || !ctx->ipc_ret_queues || !ctx->ipc_threads || !ctx->ipc_tids) {
        log_fatal("IPC thread arrays allocation failed");
        return false;
    }

    pthread_mutex_init(&ctx->main_mutex, NULL);
    pthread_cond_init(&ctx->main_cond, NULL);
    atomic_init(&ctx->main_wakeup, false);

    for (int i = 0; i < n; i++) {
        ctx->ipc_cmd_queues[i] = msg_queue_create(MSG_QUEUE_DEFAULT_CAPACITY);
        ctx->ipc_ret_queues[i] = msg_queue_create(MSG_QUEUE_DEFAULT_CAPACITY);
        if (!ctx->ipc_cmd_queues[i] || !ctx->ipc_ret_queues[i]) {
            log_fatal("msg_queue_create failed for worker %d", i);
            return false;
        }

        ctx->ipc_threads[i] = ipc_thread_ctx_create(i, ctx->worker_pool,
                                                     ctx->ipc_cmd_queues[i],
                                                     ctx->ipc_ret_queues[i],
                                                     &ctx->main_cond);
        if (!ctx->ipc_threads[i]) {
            log_fatal("ipc_thread_ctx_create failed for worker %d", i);
            return false;
        }

        if (pthread_create(&ctx->ipc_tids[i], NULL, ipc_thread_loop, ctx->ipc_threads[i]) != 0) {
            log_fatal("pthread_create failed for IPC thread %d", i);
            return false;
        }
    }

    return true;
}

void destroy_ipc_threads(AppContext *ctx) {
    int n = ctx->worker_pool ? ctx->worker_pool->num_workers : 0;
    for (int i = 0; i < n; i++) {
        if (ctx->ipc_threads && ctx->ipc_threads[i]) {
            ipc_thread_stop(ctx->ipc_threads[i]);
        }
        if (ctx->ipc_tids) {
            pthread_join(ctx->ipc_tids[i], NULL);
        }
        if (ctx->ipc_threads && ctx->ipc_threads[i]) {
            ipc_thread_ctx_destroy(ctx->ipc_threads[i]);
        }
        if (ctx->ipc_cmd_queues && ctx->ipc_cmd_queues[i]) {
            msg_queue_destroy(ctx->ipc_cmd_queues[i]);
        }
        if (ctx->ipc_ret_queues && ctx->ipc_ret_queues[i]) {
            msg_queue_destroy(ctx->ipc_ret_queues[i]);
        }
    }
    free(ctx->ipc_cmd_queues);
    free(ctx->ipc_ret_queues);
    free(ctx->ipc_threads);
    free(ctx->ipc_tids);
    pthread_mutex_destroy(&ctx->main_mutex);
    pthread_cond_destroy(&ctx->main_cond);
}

void stop_all_ipc_threads(AppContext *ctx) {
    int n = ctx->worker_pool ? ctx->worker_pool->num_workers : 0;
    for (int i = 0; i < n; i++) {
        send_stop_to_ipc(ctx, i);
    }
}

/* ================================================================
 * wait_for_ipc_messages: cond_wait with timeout
 * ================================================================ */

static void wait_for_ipc_messages(AppContext *ctx, int timeout_ms) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (timeout_ms % 1000) * 1000000;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&ctx->main_mutex);
    pthread_cond_timedwait(&ctx->main_cond, &ctx->main_mutex, &ts);
    pthread_mutex_unlock(&ctx->main_mutex);
}

/* ================================================================
 * v15.6.0: 目录任务完成屏障推进（P0-001/P0-002，仅主线程调用）
 * ================================================================ */

/**
 * @brief  推进所有 slot 的在途任务屏障状态机
 * @param  ctx  AppContext*  应用上下文
 * @return void
 *
 * @note   对 task_state >= DT_BATCHES_RECEIVED（已收 FINISH）的 slot 逐级检查：
 *         1. batches_processed == batches_received → DT_BATCHES_PROCESSED
 *            （空目录 FINISH 时 0==0 直通；协议空批次照常计数、自然闭环）
 *         2. output_pending[slot] == 0（该任务输出全部 COMMITTED）→ 此刻才
 *            dpbin_append（dpbin 写入时机从 dispatch 迁移至此），随后
 *            pending_tasks--、task_state=DT_COMPLETED、Worker 置 IDLE
 *            （Worker 只有此时才可被再次派发）
 *         dpbin 跳过条件沿用原派发处语义：st_dev==0 的重入队任务（无 stat）不写。
 */
static int advance_task_barriers(AppContext *ctx) {
    int completed = 0;
    for (int i = 0; i < ctx->worker_pool->num_workers; i++) {
        WorkerSlot *slot = &ctx->worker_pool->slots[i];
        /* 只处理两个中间态：DT_COMPLETED 已完结（必须跳过，防止重复 dpbin/pending_tasks--），
         * DT_NONE/DT_SCANNING 未到 FINISH */
        if (slot->task_state != DT_BATCHES_RECEIVED && slot->task_state != DT_BATCHES_PROCESSED)
            continue;

        /* DT_BATCHES_RECEIVED → DT_BATCHES_PROCESSED */
        if (atomic_load(&slot->batches_processed) < atomic_load(&slot->batches_received))
            continue;
        slot->task_state = DT_BATCHES_PROCESSED;

        /* DT_BATCHES_PROCESSED → DT_COMPLETED：等输出线程 COMMITTED */
        if (ctx->output_pending && atomic_load(&ctx->output_pending[i]) > 0)
            continue;

        /* v15.6.0: dpbin 落盘前先把已发现条目的 pbin 记录持久化（内存批量缓冲 +
         * stdio 缓冲全部刷出）。否则崩溃后本目录已在 completed_set 而其子目录
         * 的 pbin 记录丢失，恢复时被 completed_set 剪枝 → 子树永久漏扫。
         * record_batch 仅主线程读写（追加发生在 drain 回调），此处flush无竞态。 */
        record_path_batch_flush(&ctx->cfg, &ctx->state, &ctx->record_batch);
        if (ctx->state.write_slice_file) fflush(ctx->state.write_slice_file);

        /* v15.6.0: dpbin/dfpbin 与 pbin 同生共死——completed_set 剪枝的安全前提是
         * "已完成目录的子树记录可在恢复时从 pbin 泵送闭环"。pbin 全量记录后
         * （--clean 除外，record_path 内部早退），dpbin 同样仅在非 clean 时写入；
         * 两者必须同时存在或同时缺席，否则崩溃续传后剪枝失去 pbin 泵送兜底
         * → 子树永久漏扫（v15.6.0 回归实测 58 文件）。 */
        if (slot->current_st.st_dev != 0 && !ctx->cfg.clean) {
            if (ctx->hist_pump_state == HIST_PUMP_OLD) {
                /* v15.6.0（P0-003 fpbin/dfpbin 原子对）：HIST_PUMP_OLD 阶段完成的
                 * 父目录写 dfpbin 而非 dpbin。耐久性顺序与 pbin→dpbin 同款：
                 * dfpbin_append 前先 fpbin_flush（内存缓冲刷出 + fflush 活跃分片）
                 * ——父目录"完成"前其新发现子目录必须已在 fpbin 落盘，否则崩溃后
                 * 父目录在 dfpbin、子目录丢失 → 恢复时父目录被剪枝 → 子树漏扫。 */
                fpbin_flush(ctx);
                dfpbin_append(ctx, slot->current_path, &slot->current_st);
                /* dfpbin 同样刷出 stdio 缓冲，保证 fpbin→dfpbin 的落盘顺序 */
                if (ctx->dfpbin_slice_file) fflush(ctx->dfpbin_slice_file);
            } else {
                dpbin_append(ctx, slot->current_path, &slot->current_st);
                /* dpbin 同样刷出 stdio 缓冲，保证 pbin→dpbin 的落盘顺序 */
                if (ctx->dpbin_slice_file) fflush(ctx->dpbin_slice_file);
            }
            /* v15.6.0（P0-004）：运行期同步 completed_set，
             * 作为 enqueue_dir 差集剪枝的运行时依据（恢复时由 dpbin 加载重建） */
            if (ctx->completed_set) {
                uint8_t fp[FP_SIZE];
                fp_compute(slot->current_path, slot->current_st.st_dev,
                           slot->current_st.st_ino, fp);
                fp_set_insert(ctx->completed_set, fp);
            }
        }
        atomic_fetch_sub(&ctx->pending_tasks, 1);
        slot->task_state = DT_COMPLETED;
        atomic_store(&slot->state, WORKER_STATE_IDLE); /* v15.1.0：仅此时可被再次派发 */
        completed++;
        log_debug_v(202608202330UL, "[Barrier] Worker %d task COMPLETED: %s (pending_tasks=%ld)",
                    i, path_log_mask(slot->current_path), atomic_load(&ctx->pending_tasks));
    }
    return completed;
}

/* ================================================================
 * Main loop: Message Bus (v13.0.0) — pthread_cond_wait, no epoll
 * ================================================================ */

void main_loop_run(AppContext *ctx) {
    /* Create thread pool completion eventfd */
    ctx->event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (ctx->event_fd < 0) {
        log_fatal("eventfd creation failed");
        return;
    }

    ctx->thread_pool = thread_pool_create(ctx->cfg.master_threads, ctx->event_fd,
                                          batch_dedup_worker, ctx, &ctx->main_cond);
    if (!ctx->thread_pool) {
        log_fatal("Thread pool creation failed");
        close(ctx->event_fd);
        ctx->event_fd = -1;
        return;
    }

    ctx->running = true;

    /* v15.6.0: 自适应等待——上一轮有任何进展（消息/批次/屏障完结）时下一轮
     * 零等待直接处理，避免 cond 信号不排队导致的丢失唤醒把流水线拖到
     * 100ms 粒度（小目录场景吞吐骤降 10 倍+，回归实测）；空闲时才睡满 100ms。 */
    bool made_progress = false;

    while (ctx->running) {
        /* 1. Block on cond_wait for IPC messages (100ms timeout; 0 if busy) */
        wait_for_ipc_messages(ctx, made_progress ? 0 : 100);

        /* v15.0.2 debug: 每 ~10s 打印一次主循环状态 */
        static int loop_counter = 0;
        if (++loop_counter >= 100) {
            loop_counter = 0;
            log_info("[MainLoop] pending_tasks=%ld pending_batches=%ld hist_state=%d dispatch_queue=%zu",
                     atomic_load(&ctx->pending_tasks), atomic_load(&ctx->pending_batches),
                     ctx->hist_pump_state, dispatch_queue_count(&ctx->dispatch_queue));
        }

        /* 2. Drain all IPC return queues */
        bool progress_this_round = false;
        for (int i = 0; i < ctx->worker_pool->num_workers; i++) {
            pthread_mutex_lock(&ctx->ipc_ret_queues[i]->mutex);
            size_t head = ctx->ipc_ret_queues[i]->head;
            size_t tail = ctx->ipc_ret_queues[i]->tail;
            pthread_mutex_unlock(&ctx->ipc_ret_queues[i]->mutex);
            log_debug("[Main] ret_queue[%d] head=%zu tail=%zu queue=%p", i, head, tail, (void*)ctx->ipc_ret_queues[i]);
            IpcThreadMsg msg;
            int drained = 0;
            while (msg_queue_recv(ctx->ipc_ret_queues[i], &msg)) {
                handle_return_message(ctx, &msg);
                drained++;
            }
            if (drained > 0) {
                log_debug("[Main] Drained %d messages from ret_queue[%d]", drained, i);
                progress_this_round = true;
            }
        }

        /* 3. Drain thread pool completed batches */
        if (drain_completed_batches(ctx) > 0) progress_this_round = true;
        /* Also drain eventfd counter to avoid stale notifications */
        if (ctx->event_fd >= 0) {
            uint64_t n;
            while (read(ctx->event_fd, &n, sizeof(n)) > 0) {
                if (drain_completed_batches(ctx) > 0) progress_this_round = true;
            }
        }

        /* 3.5 v15.6.0: 推进目录任务完成屏障（P0-001/P0-002）——
         * 已收 FINISH 的任务等待批次处理完 + 输出 COMMITTED 后才完结 */
        if (advance_task_barriers(ctx) > 0) progress_this_round = true;

        /* 4. Pump historical pbin directories */
        if (ctx->hist_pump_state == HIST_PUMP_OLD || ctx->hist_pump_state == HIST_PUMP_NEW) {
            pump_pbin_batch(ctx, ctx->cfg.batch_size);
        }

        /* 5. Reap zombie children */
        for (int i = 0; i < ctx->worker_pool->num_workers * 2; i++) {
            if (waitpid(-1, NULL, WNOHANG) <= 0) break;
        }

        /* 6. Replace dead workers */
        for (int i = 0; i < ctx->worker_pool->num_workers; i++) {
            WorkerSlot *slot = &ctx->worker_pool->slots[i];
            if (!atomic_load(&slot->is_alive) && slot->pid == -1) {
                /* v15.6.1（P0-102）：屏障接管态（DT_BATCHES_*）的任务仍可由
                 * advance_task_barriers 完结（dpbin 需要 current_path/current_st）——
                 * 必须等屏障销账后才允许替换复用 slot，否则 dpbin 会拿到空路径 */
                if (slot->task_state == DT_BATCHES_RECEIVED || slot->task_state == DT_BATCHES_PROCESSED)
                    continue;
                cleanup_dead_worker_slot(ctx, i, true);
                /* v15.6.1（P0-106）：Worker 替换是生命周期关键事件，默认级别可见 */
                log_warn("[Replace] Replacing dead worker %d", i);
                /* v15.6.1（P0-107b）：替换走预备役池，运行期零 fork；
                 * 失败（spare 耗尽）下一轮重试，由有效进展看门狗兜底 */
                if (worker_pool_replace(ctx->worker_pool, i)) {
                    send_replace_to_ipc(ctx, i, slot->fd_cmd, slot->fd_data, slot->fd_ctrl, slot->pid);
                }
            }
        }

        /* 7. Dispatch from queue */
        dispatch_from_queue(ctx);

        /* 7.5 v15.5.8: dspill loader — 队列降到 LOW_WATER 时从兜底文件回填
         * （替代已废的 pbin 滑动窗口；dspill 仅含 HIGH_WATER 跳推目录） */
        if (dispatch_queue_count(&ctx->dispatch_queue) <= DISPATCH_QUEUE_LOW_WATER
            && ctx->dspill_fp) {
            load_dirs_from_dspill(ctx, DISPATCH_QUEUE_LOAD_BATCH);
        }
        /* v15.6.0（P0-004）：dspill 定时刷盘挂点（1000 条或 1 秒，whichever first） */
        dspill_flush_check(ctx, false);

        /* 8. Termination check */
        /* v15.6.1（P0-104）：账目不变量——pending_tasks<0 即销账 bug，必须立即暴露，
         * 不得死等（生产事故：-6 使完结条件永不成立，空转 54 小时）。
         * 杀光存活 Worker 后 _exit(2)（严重失败），不走优雅退出（账目已不可信）。 */
        long pending_now = atomic_load(&ctx->pending_tasks);
        if (pending_now < 0) {
            log_fatal("[MainLoop] INVARIANT VIOLATION: pending_tasks=%ld < 0 —— "
                      "任务账目错误，终止运行", pending_now);
            ctx->state.has_error = true;
            for (int i = 0; i < ctx->worker_pool->num_workers; i++) {
                WorkerSlot *slot = &ctx->worker_pool->slots[i];
                if (atomic_load(&slot->is_alive) && slot->pid > 0)
                    kill(slot->pid, SIGKILL);
            }
            _exit(2);
        }
        if (pending_now <= 0
            && atomic_load(&ctx->pending_batches) == 0
            && dispatch_queue_count(&ctx->dispatch_queue) == 0
            && ctx->hist_pump_state == HIST_PUMP_DONE) {
            /* v15.5.8 完结硬性断言：dspill 兜底文件必须先排空到 EOF。
             * 游标未到 EOF 说明仍有 HIGH_WATER 跳推目录未回填——回填后继续扫描，
             * 不得完结；回填无法推进（记录损坏/持续 push 失败）则残留即丢失，
             * 记熔断清单（skipped_count>0 → 非零退出码），不允许静默成功。 */
            if (ctx->dspill_fp) {
                int loaded = load_dirs_from_dspill(ctx, DISPATCH_QUEUE_LOAD_BATCH);
                if (loaded > 0 || dispatch_queue_count(&ctx->dispatch_queue) > 0)
                    continue;
                /* v15.6.0（P0-004）：强制刷盘后再统计残留字节（刷盘已改缓冲批量） */
                dspill_flush_check(ctx, true);
                char *spill_path = get_dspill_filename(ctx->cfg.progress_base);
                if (spill_path) {
                    struct stat st;
                    if (stat(spill_path, &st) == 0 && st.st_size > (off_t)ctx->dspill_read_offset) {
                        long residue = (long)(st.st_size - (off_t)ctx->dspill_read_offset);
                        log_error("[MainLoop] DSPILL_RESIDUE: %ld unread bytes in %s — 跳推目录丢失",
                                  residue, spill_path);
                        circuit_breaker_record(ctx, "DSPILL_RESIDUE", ctx->cfg.target_path, 0, (int)residue);
                    }
                    free(spill_path);
                }
            }
            worker_pool_stop_all(ctx->worker_pool);
            stop_all_ipc_threads(ctx);
            ctx->running = false;
        }

        /* v15.6.0: 本轮有进展则下一轮零等待（流水线满速），无进展才睡满 100ms */
        made_progress = progress_this_round;
    }

    drain_completed_batches(ctx);
    record_path_batch_flush(&ctx->cfg, &ctx->state, &ctx->record_batch);

    thread_pool_destroy(ctx->thread_pool);
    ctx->thread_pool = NULL;
    close(ctx->event_fd);
    ctx->event_fd = -1;
    destroy_ipc_threads(ctx);
}

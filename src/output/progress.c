/**
 * @file progress.c
 * @brief 进度文件（pbin/spbin/fpbin）的写入、归档、恢复与生命周期管理
 *
 * 核心设计哲学：
 * - 同构分片：pbin、fpbin、dpbin 采用完全相同的物理格式
 * - 页脚自描述：已封口分片末尾自带 Footer（magic + row_count + crc），无需外部索引
 * - 崩溃恢复：Footer 优先，dpbin 提供差分集合用于续传
 *
 * 进度文件格式（以 --progress-file=task1 为例）：
 * - task1_000000.pbin  已封口的已完成记录分片
 * - task1.dpbin_000000 本次会话的目录完成日志（临时，正常结束后删除）
 * - task1.spbin        跳过记录（熔断设备上的目录）
 * - task1.fpbin_000XXX 恢复期间隔离新发现子目录的临时分片
 * - task1.archive      zlib 压缩的历史分片归档
 * - task1.config       会话配置快照（兼容保留，权威状态以 manifest 为准）
 * - task1.manifest     Run manifest（v15.6.0，P0-008）：运行完整性标记 + baseline_eligible
 */
#include "progress.h"
#include "utils.h"
#include "archive_format.h"
#include "msg_format.h"
#include "msg_queue.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <zlib.h>
#include <stdint.h>
#include <dirent.h>
#include <stdatomic.h>
#include "main_loop.h"
#include "log.h"

/* ================================================================
 * Filename helpers
 * ================================================================ */

/**
 * @brief  生成 pbin 分片文件名（{base}_000000.pbin）
 * @param  base   const char*   进度文件前缀，不能为空
 * @param  index  unsigned long 分片编号，取值范围: >= 0
 * @return char*  动态分配的字符串，调用方负责 free
 */
char *get_slice_filename(const char *base, unsigned long index) {
    char *name = safe_malloc(strlen(base) + 32);
    sprintf(name, "%s_%06lu.pbin", base, index);
    return name;
}

/**
 * @brief  生成 dspill 派发兜底文件名（{base}.dspill，v15.5.8）
 * @param  base  const char*  进度文件前缀，不能为空
 * @return char*  动态分配的字符串，调用方负责 free
 */
char *get_dspill_filename(const char *base) {
    char *name = safe_malloc(strlen(base) + 32);
    sprintf(name, "%s.dspill", base);
    return name;
}

/**
 * @brief  生成归档文件名（{base}.archive）
 * @param  base  const char*  进度文件前缀，不能为空
 * @return char*  动态分配的字符串，调用方负责 free
 */
char *get_archive_filename(const char *base) {
    char *name = safe_malloc(strlen(base) + 32);
    sprintf(name, "%s.archive", base);
    return name;
}

/**
 * @brief  生成归档在写副本文件名（{base}.archive.new，v15.6.0，P0-008 原子切换）
 * @param  base  const char*  进度文件前缀，不能为空
 * @return char*  动态分配的字符串，调用方负责 free
 *
 * @note   运行期所有 archive 块追加到 .new；finalize 校验通过后 rename 覆盖
 *         {base}.archive，旧基准保留为 {base}.archive.prev。
 */
char *get_archive_new_filename(const char *base) {
    char *name = safe_malloc(strlen(base) + 32);
    sprintf(name, "%s.archive.new", base);
    return name;
}

/**
 * @brief  生成旧基准归档保留文件名（{base}.archive.prev，v15.6.0，P0-008）
 * @param  base  const char*  进度文件前缀，不能为空
 * @return char*  动态分配的字符串，调用方负责 free
 */
char *get_archive_prev_filename(const char *base) {
    char *name = safe_malloc(strlen(base) + 32);
    sprintf(name, "%s.archive.prev", base);
    return name;
}

/**
 * @brief  生成 spbin 文件名（{base}.spbin）
 * @param  base  const char*  进度文件前缀，不能为空
 * @return char*  动态分配的字符串，调用方负责 free
 */
char *get_spbin_filename(const char *base) {
    char *name = safe_malloc(strlen(base) + 32);
    sprintf(name, "%s.spbin", base);
    return name;
}

/**
 * @brief  生成 fpbin 分片文件名（{base}.fpbin_000000）
 * @param  base   const char*   进度文件前缀，不能为空
 * @param  index  unsigned long 分片编号，取值范围: >= 0
 * @return char*  动态分配的字符串，调用方负责 free
 */
char *get_fpbin_slice_filename(const char *base, unsigned long index) {
    char *name = safe_malloc(strlen(base) + 48);
    sprintf(name, "%s.fpbin_%06lu", base, index);
    return name;
}

/**
 * @brief  生成 fpbin 索引文件名（{base}.fpbin.idx）
 * @param  base  const char*  进度文件前缀，不能为空
 * @return char*  动态分配的字符串，调用方负责 free
 */
char *get_fpbin_index_filename(const char *base) {
    char *name = safe_malloc(strlen(base) + 32);
    sprintf(name, "%s.fpbin.idx", base);
    return name;
}

/**
 * @brief  将 stat::st_mode 转换为 dirent::d_type 等价值
 * @param  mode  mode_t  文件模式位
 * @return unsigned char  对应的 d_type 值（DT_REG/DT_DIR/DT_LNK/...），未知时返回 DT_UNKNOWN
 */
/* ================================================================
 * 其他辅助
 * ================================================================ */

/**
 * @brief  将当前会话配置快照保存到磁盘
 * @param  cfg  const Config*  全局配置指针，不能为空
 * @return void
 *
 * @note   写入 {base}.config 文件，包含：目标路径、输出文件、归档策略、CSV 模式、启动时间等。
 *         --clean 模式不创建任何进度文件。
 */
void save_config_to_disk(const Config* cfg) {
    if (!cfg->progress_base) return;
    if (cfg->clean) return; /* --clean mode should not leave any progress files */
    char config_path[1024];
    snprintf(config_path, sizeof(config_path), "%s.config", cfg->progress_base);
    FILE *fp = fopen(config_path, "w");
    if (!fp) return;
    fprintf(fp, "path=%s\n", cfg->target_path);
    if (cfg->output_file) fprintf(fp, "output=%s\n", cfg->output_file);
    if (cfg->output_split_dir) fprintf(fp, "output_split=%s\n", cfg->output_split_dir);
    fprintf(fp, "start_time=%ld\n", time(NULL));
    fprintf(fp, "archive=%d\n", cfg->archive);
    fprintf(fp, "clean=%d\n", cfg->clean);
    fprintf(fp, "csv=%d\n", cfg->csv);
    fprintf(fp, "status=Running\n");
    fclose(fp);
}

/**
 * @brief  任务结束时的进度收尾处理
 * @param  cfg    const Config*   全局配置指针，不能为空
 * @param  state  RuntimeState*   运行时状态指针，不能为空
 * @return bool  返回 archive 原子切换校验结果（未启用 archive 或 --clean 时恒 true），
 *               供 manifest_finalize 判定 baseline_eligible
 *
 * @note   非 --clean 模式：
 *         1. 调用 finalize_archive 封口活跃分片并归档（v15.6.0：含 .new → 校验 →
 *            rename 原子切换，旧基准保留为 .archive.prev）
 *         2. 删除本次会话的 dpbin 临时文件
 *         3. 追加状态行到 .config（Success/Incomplete + 结束时间）——
 *            .config 仅为兼容保留，权威状态以 {base}.manifest 为准（v15.6.0，P0-008）
 *         --clean 模式：
 *         关闭并删除活跃分片文件，不保留任何进度记录。
 */
bool finalize_progress(const Config *cfg, RuntimeState *state) {
    if (!cfg->clean) {
        bool archive_ok = finalize_archive(cfg, state);
        dpbin_delete_all(cfg->progress_base);
        /* v15.5.0: idx abolished, no cursor to write */
        if (cfg->progress_base) {
            char config_path[1024];
            snprintf(config_path, sizeof(config_path), "%s.config", cfg->progress_base);
            FILE *fp = fopen(config_path, "a");
            if (fp) {
                if (state->has_error) {
                    fprintf(fp, "status=Incomplete\n");
                    fprintf(fp, "error=DeviceMeltdown\n");
                } else {
                    fprintf(fp, "status=Success\n");
                }
                fprintf(fp, "end_time=%ld\n", time(NULL));
                fclose(fp);
            }
        }
        return archive_ok;
    } else {
        /* --clean mode: do not create any new progress files */
        if (state->write_slice_file) {
            fclose(state->write_slice_file);
            state->write_slice_file = NULL;
            char *src_path = get_slice_filename(cfg->progress_base, state->write_slice_index);
            unlink(src_path);
            free(src_path);
        }
        return true;
    }
}

/**
 * @brief  清理所有进度文件（--clean 或 --runone 时调用）
 * @param  cfg    const Config*   全局配置指针，不能为空
 * @param  state  RuntimeState*   运行时状态指针，不能为空
 * @return void
 *
 * @note   删除：所有分片文件、归档文件、spbin、dpbin、dfpbin、dspill、错误日志、
 *         config、fpbin 索引和分片、以及兼容旧版本的 progress.fpbin。
 *         注意：仅删除到 write_slice_index + 200 为止的 pbin 分片，保留可能更远的残留。
 */
void cleanup_progress(const Config *cfg, RuntimeState *state) {
    /* Always clean up slice files on --clean; on --archive they were already archived */
    if (cfg->clean || cfg->archive) {
        for (unsigned long i = 0; i <= state->write_slice_index + 200; i++) {
            char *slice_path = get_slice_filename(cfg->progress_base, i);
            unlink(slice_path);
            free(slice_path);
        }
    }
    /* Clean up any dpbin slices (session temporary) */
    dpbin_delete_all(cfg->progress_base);
    /* v15.6.0（P0-003）：清理 dfpbin 原子对残留 */
    dfpbin_delete_all(cfg->progress_base);
    /* v15.6.0（P0-004）：dspill 已纳入恢复（restore_progress 读取），
     * 仅在 --clean/--runone 强制重跑时随其他进度文件一并删除 */
    if (cfg->progress_base) {
        char *spill_path = get_dspill_filename(cfg->progress_base);
        unlink(spill_path);
        free(spill_path);
    }

    char *arch_path = get_archive_filename(cfg->progress_base);
    if (cfg->clean) unlink(arch_path);
    free(arch_path);

    /* v15.6.0（P0-008）：archive 原子切换的在写副本与旧基准保留件 */
    char *arch_new = get_archive_new_filename(cfg->progress_base);
    if (cfg->clean) unlink(arch_new);
    free(arch_new);
    char *arch_prev = get_archive_prev_filename(cfg->progress_base);
    if (cfg->clean) unlink(arch_prev);
    free(arch_prev);

    /* v15.6.0（P0-008）：manifest 随 --clean/--runone 一并清理
     * （runone 启动后会由 manifest_write_running 重建 Running 态） */
    if (cfg->clean || cfg->runone) {
        char mpath[1024];
        snprintf(mpath, sizeof(mpath), "%s.manifest", cfg->progress_base);
        unlink(mpath);
        snprintf(mpath, sizeof(mpath), "%s.manifest.new", cfg->progress_base);
        unlink(mpath);
    }

    char *spbin_path = get_spbin_filename(cfg->progress_base);
    unlink(spbin_path);
    free(spbin_path);

    char error_log[1024];
    snprintf(error_log, sizeof(error_log), "%s.error.log", cfg->progress_base);
    unlink(error_log);

    if (cfg->clean) {
        char config_path[1024];
        snprintf(config_path, sizeof(config_path), "%s.config", cfg->progress_base);
        unlink(config_path);
    }

    /* 清理残留 fpbin（基于 progress_base） */
    char *fpbin_idx = get_fpbin_index_filename(cfg->progress_base);
    unlink(fpbin_idx);
    free(fpbin_idx);
    for (unsigned long i = 0; i < 1000; i++) {
        char *fp = get_fpbin_slice_filename(cfg->progress_base, i);
        unlink(fp);
        free(fp);
    }
    /* 兼容旧版本残留 */
    unlink("progress.fpbin");
}

/**
 * @brief  获取文件锁（防止多实例同时操作同一进度文件）
 * @param  cfg    const Config*   全局配置指针，不能为空
 * @param  state  RuntimeState*   运行时状态指针，不能为空
 * @return int  返回 0 表示加锁成功；返回 -1 表示失败（文件不存在或其他进程已持有锁）
 *
 * @note   仅在 continue_mode 下生效。锁文件为 {base}.lock，使用 flock(LOCK_EX | LOCK_NB)。
 *         成功后将 fd 和路径记录到 state 中，由 release_lock 释放。
 */
int acquire_lock(const Config *cfg, RuntimeState *state) {
    if (!cfg->continue_mode) return 0;
    char *lock_path = safe_malloc(strlen(cfg->progress_base) + 32);
    sprintf(lock_path, "%s.lock", cfg->progress_base);
    state->lock_file_path = lock_path;
    int fd = open(lock_path, O_RDWR | O_CREAT, 0666);
    if (fd == -1) return -1;
    if (flock(fd, LOCK_EX | LOCK_NB) == -1) { close(fd); return -1; }
    state->lock_fd = fd;
    return 0;
}

/**
 * @brief  释放文件锁并删除锁文件
 * @param  state  RuntimeState*  运行时状态指针，不能为空
 * @return void
 */
void release_lock(RuntimeState *state) {
    if (state->lock_fd != -1) { flock(state->lock_fd, LOCK_UN); close(state->lock_fd); state->lock_fd = -1; }
    if (state->lock_file_path) { unlink(state->lock_file_path); free(state->lock_file_path); state->lock_file_path = NULL; }
}

/* ================================================================
 * Spbin memory cache
 * ================================================================ */

/**
 * @brief  向 spbin 内存缓存追加一条记录
 * @param  ctx    AppContext*         应用上下文指针，不能为空
 * @param  entry  const SpbinEntry*   要追加的跳过记录指针，不能为空
 * @return void
 *
 * @note   当缓存满时自动扩容至 2 倍容量。内存缓存用于设备恢复时快速重入队，
 *         避免重复读取 spbin 磁盘文件。
 */
void spbin_append(AppContext *ctx, const SpbinEntry *entry) {
    if (ctx->spbin_count >= ctx->spbin_capacity) {
        size_t new_cap = ctx->spbin_capacity ? ctx->spbin_capacity * 2 : 64;
        SpbinEntry *new_arr = realloc(ctx->spbin_entries, new_cap * sizeof(SpbinEntry));
        if (!new_arr) return;
        ctx->spbin_entries = new_arr;
        ctx->spbin_capacity = new_cap;
    }
    ctx->spbin_entries[ctx->spbin_count] = *entry;
    ctx->spbin_count++;
}

/**
 * @brief  spbin 统一写入入口：内存缓存 + spbin_set + 磁盘 append-only（v15.6.0，P0-005）
 * @param  ctx     AppContext*  应用上下文指针，不能为空
 * @param  path    const char*  被跳过的目录路径，不能为空
 * @param  reason  uint8_t      跳过原因码（SP_REASON_*）
 * @param  dev     dev_t        目录所在设备号
 * @return void
 *
 * @note   所有跳过路径（RET_ERROR 各 reason、目录级熔断达阈值、毒丸隔离）的统一入口。
 *         - 内存状态：PROBE_FAIL/TIMEOUT → SP_STATUS_PROBING（等待设备探测恢复）；
 *           PERMISSION/CIRCUIT_BREAKER/POISON → SP_STATUS_CONDEMNED（本次运行不再入队）。
 *         - spbin_set（path-only 指纹）兼作去重依据：同一路径重复跳过只更新既有条目
 *           （如 PROBE_FAIL 升级为 POISON）；磁盘 append-only 允许重复记录，
 *           恢复时后写覆盖先写。
 *         - 落盘立即 fflush（跳过记录是崩溃恢复依据）；无 progress_base 时仅维护内存。
 *         - 条目数达 SPBIN_MAX_ENTRIES（10 万）时告警并紧急 compaction。
 */
void spbin_write_record(AppContext *ctx, const char *path, uint8_t reason, dev_t dev) {
    if (!ctx || !path) return;

    uint8_t s_status = (reason == SP_REASON_PROBE_FAIL || reason == SP_REASON_TIMEOUT)
                       ? SP_STATUS_PROBING : SP_STATUS_CONDEMNED;
    time_t now = time(NULL);

    if (!ctx->spbin_set) {
        ctx->spbin_set = fp_set_create(4096);
    }
    bool found = false;
    if (ctx->spbin_set) {
        uint8_t fp[FP_SIZE];
        fp_compute(path, 0, 0, fp);
        if (fp_set_insert(ctx->spbin_set, fp)) {
            /* 已存在：更新 reason/timestamp/status（紧急 compaction 可能已移除
             * 内存条目，找不到时回退为追加新条目） */
            for (size_t i = 0; i < ctx->spbin_count; i++) {
                if (strcmp(ctx->spbin_entries[i].path, path) == 0) {
                    ctx->spbin_entries[i].reason = reason;
                    ctx->spbin_entries[i].dev = (uint64_t)dev;
                    ctx->spbin_entries[i].timestamp = now;
                    ctx->spbin_entries[i].s_status = s_status;
                    found = true;
                    break;
                }
            }
        }
    }
    if (!found) {
        SpbinEntry entry = {0};
        entry.path = strdup(path);
        if (!entry.path) return;
        entry.dev = (uint64_t)dev;
        entry.reason = reason;
        entry.timestamp = now;
        entry.retry_count = 0;
        entry.s_status = s_status;
        spbin_append(ctx, &entry);
    }

    /* 落盘（append-only + fflush） */
    if (ctx->cfg.progress_base) {
        SpbinEntry disk = {0};
        disk.path = (char *)path;
        disk.dev = (uint64_t)dev;
        disk.reason = reason;
        disk.timestamp = now;
        spbin_file_append(ctx->cfg.progress_base, &disk);
    }

    if (ctx->spbin_count >= SPBIN_MAX_ENTRIES) {
        log_warn("[spbin] 条目数达上限 %d，触发紧急 compaction", SPBIN_MAX_ENTRIES);
        spbin_compact(ctx);
    }
}

/**
 * @brief  spbin compaction：过滤已恢复（RECOVERED）条目，重写 {base}.spbin（v15.6.0，P0-005）
 * @param  ctx  AppContext*  应用上下文指针，不能为空
 * @return void
 *
 * @note   spbin 运行期保持 append-only（不随机改写），正常退出时由本函数清理：
 *         已恢复条目不再写入新文件，并从内存数组移除。tmp + rename 保证崩溃安全：
 *         compaction 中途崩溃最多损失本次 compaction，旧文件仍在。
 *         条目数达上限时的紧急 compaction 复用本函数。
 */
void spbin_compact(AppContext *ctx) {
    if (!ctx || !ctx->cfg.progress_base || ctx->cfg.clean) return;

    char *spbin_path = get_spbin_filename(ctx->cfg.progress_base);
    char *tmp_path = safe_malloc(strlen(spbin_path) + 16);
    sprintf(tmp_path, "%s.tmp", spbin_path);

    FILE *fp = fopen(tmp_path, "wb");
    if (!fp) {
        log_warn("[spbin] compaction 无法创建临时文件: %s", tmp_path);
        free(tmp_path);
        free(spbin_path);
        return;
    }

    size_t out = 0;
    for (size_t i = 0; i < ctx->spbin_count; i++) {
        SpbinEntry *e = &ctx->spbin_entries[i];
        if (e->s_status == SP_STATUS_RECOVERED) {
            free(e->path);
            e->path = NULL;
            continue;
        }
        spbin_file_write_entry(fp, e);
        if (out != i) ctx->spbin_entries[out] = *e;
        out++;
    }
    fflush(fp);
    fclose(fp);

    if (rename(tmp_path, spbin_path) != 0) {
        log_error("[spbin] compaction rename 失败: %s -> %s", tmp_path, spbin_path);
        unlink(tmp_path);
    }
    ctx->spbin_count = out;
    free(tmp_path);
    free(spbin_path);
}

/**
 * @brief  设备恢复后，将 spbin 中该设备的积压路径重新入队扫描
 * @param  ctx  AppContext*  应用上下文指针，不能为空
 * @param  dev  dev_t        已恢复的设备号
 * @return void
 *
 * @note   遍历 spbin_entries 数组，找到匹配 dev 且状态为 SP_STATUS_PROBING 的条目
 *         （PERMISSION/CIRCUIT_BREAKER/POISON 与未超窗条目为 CONDEMNED，不重入队），
 *         重入队后在内存标记 SP_STATUS_RECOVERED，由正常退出时的 spbin_compact 过滤落盘。
 *         v15.6.0：统一经 dispatch_queue/dspill 重入队，由 dispatch_from_queue 负责
 *         pending_tasks 账目、epoch 与 Worker 调度。
 *         注意必须绕过 enqueue_dir 的集合去重：运行期出错的目录在首次发现时已登记
 *         enqueued_set（P0-007 起出错目录不重入队、等待设备恢复），若走 enqueue_dir
 *         会被去重拦截，设备恢复后永远无法重扫。恢复期目录由 spbin_set 拦截泵送，
 *         不会与泵送重复入队。
 */
void spbin_requeue_recovered(AppContext *ctx, dev_t dev) {
    for (size_t i = 0; i < ctx->spbin_count; i++) {
        SpbinEntry *e = &ctx->spbin_entries[i];
        if (e->dev == (uint64_t)dev && e->s_status == SP_STATUS_PROBING) {
            /* v15.6.0: 设备恢复后的统一重入队不再旁路直发 CMD_SCAN（旁路不占 BUSY
             * 态、不登记 epoch，会破坏 epoch 校验导致 pending_tasks 泄漏卡死），
             * 统一走 dispatch_queue/dspill，由 dispatch_from_queue 负责账目。 */
            if (dispatch_queue_count(&ctx->dispatch_queue) < DISPATCH_QUEUE_HIGH_WATER
                || !ctx->cfg.progress_base) {
                char *dup = strdup(e->path);
                if (!dup || !dispatch_queue_push(&ctx->dispatch_queue, dup, NULL)) {
                    free(dup);
                    dspill_append(ctx, e->path, NULL);
                }
            } else {
                dspill_append(ctx, e->path, NULL);
            }
            e->s_status = SP_STATUS_RECOVERED;
        }
    }
}

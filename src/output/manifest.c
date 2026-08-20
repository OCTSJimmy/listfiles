/**
 * @file manifest.c
 * @brief Run manifest（{base}.manifest）生命周期管理（v15.6.0，P0-008）
 *
 * 核心设计哲学：
 * - 原子化写入：所有更新走 {base}.manifest.new → fflush+fsync → rename 覆盖，
 *   崩溃最多损失本次更新，读者永远看到完整旧版本或完整新版本；
 * - 单行覆盖语义：文本 key=value，每键仅一行（读者按后写覆盖先写防御重复键），
 *   替代 .config 追加写积累多行 status 导致 strstr/逐行匹配误判的旧 bug；
 * - 完整性由工具强制：baseline_eligible=1 必须同时满足——status=Success、
 *   spbin 残留为 0、dspill 已排空、无 fpbin/dfpbin 残留、archive（若启用）
 *   写出校验通过、输出尾部完整（最后一字节为 '\n'）、且本轮非盲信运行。
 *
 * .config 继续按原样写入（兼容性），但一切状态判断以 manifest 为准。
 */
#include "manifest.h"
#include "app_context.h"
#include "progress.h"
#include "utils.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

/* ================================================================
 * 内部辅助
 * ================================================================ */

/**
 * @brief  生成 manifest 及其临时文件路径
 * @param  base     const char*  进度文件前缀，不能为空
 * @param  out      char*        输出 {base}.manifest 路径缓冲区，不能为空
 * @param  out_new  char*        输出 {base}.manifest.new 路径缓冲区，不能为空
 * @param  sz       size_t       缓冲区大小（两个缓冲区同尺寸）
 * @return void
 */
static void manifest_paths(const char *base, char *out, char *out_new, size_t sz) {
    snprintf(out, sz, "%s.manifest", base);
    snprintf(out_new, sz, "%s.manifest.new", base);
}

/**
 * @brief  原子写入 manifest 内容（.new → fflush+fsync → rename 覆盖）
 * @param  base     const char*  进度文件前缀，不能为空
 * @param  content  const char*  完整文本内容，不能为空
 * @return bool  返回 true 表示写入并切换成功；false 表示失败（旧 manifest 不受影响）
 */
static bool manifest_write_atomic(const char *base, const char *content) {
    char path[1200], path_new[1200];
    manifest_paths(base, path, path_new, sizeof(path));

    FILE *fp = fopen(path_new, "w");
    if (!fp) {
        log_error("[manifest] 无法创建临时文件: %s", path_new);
        return false;
    }
    fputs(content, fp);
    fflush(fp);
    int fd = fileno(fp);
    if (fd >= 0) fsync(fd);
    fclose(fp);

    if (rename(path_new, path) != 0) {
        log_error("[manifest] rename 失败: %s -> %s", path_new, path);
        unlink(path_new);
        return false;
    }
    return true;
}

/**
 * @brief  检查 fpbin/dfpbin 原子对是否存在任何残留分片
 * @param  base  const char*  进度文件前缀，不能为空
 * @return bool  存在任何残留（含 fpbin.idx）返回 true
 *
 * @note   baseline_eligible 的严格条件之一：fpbin 已转正、无残留工作区。
 *         残留说明上次恢复未闭环，本次结果不能作为盲信基准。
 */
static bool fpbin_pair_residue_exists(const char *base) {
    char *idx = get_fpbin_index_filename(base);
    bool found = (access(idx, F_OK) == 0);
    free(idx);
    for (unsigned long i = 0; !found && i < 1024; i++) {
        char *p = get_fpbin_slice_filename(base, i);
        found = (access(p, F_OK) == 0);
        free(p);
    }
    for (unsigned long i = 0; !found && i < 1024; i++) {
        char *p = get_dfpbin_slice_filename(base, i);
        found = (access(p, F_OK) == 0);
        free(p);
    }
    return found;
}

/**
 * @brief  校验输出尾部完整性并统计最终输出偏移
 * @param  cfg         const Config*        全局配置指针，不能为空
 * @param  state       const RuntimeState*  运行时状态指针，不能为空
 * @param  out_offset  unsigned long*       输出参数，返回输出文件最终字节数
 * @return bool  返回 true 表示输出尾部完整（空文件/stdout 视为完整）
 *
 * @note   baseline_eligible 的严格条件之一：输出文件最后一字节必须为 '\n'
 *         （崩溃截断的输出尾部不完整，不能作为盲信基准）。
 *         分片模式校验当前（最大编号）分片；stdout 无法校验，视为完整。
 *         调用前调用方须已关停异步输出线程并 fflush，保证读到最终内容。
 */
static bool output_tail_complete(const Config *cfg, const RuntimeState *state,
                                 unsigned long *out_offset) {
    *out_offset = 0;
    char slice_path[1200];
    const char *path = NULL;
    if (cfg->is_output_split_dir && cfg->output_split_dir) {
        snprintf(slice_path, sizeof(slice_path), "%s/" OUTPUT_SLICE_FORMAT,
                 cfg->output_split_dir, state->output_slice_num);
        path = slice_path;
    } else if (cfg->is_output_file && cfg->output_file) {
        path = cfg->output_file;
    } else {
        return true; /* stdout 无法校验，视为完整 */
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) return true; /* 无输出文件（空结果），视为完整 */
    off_t sz = lseek(fd, 0, SEEK_END);
    if (sz > 0) *out_offset = (unsigned long)sz;
    if (sz <= 0) {
        close(fd);
        return true;
    }
    char c = 0;
    bool ok = (lseek(fd, sz - 1, SEEK_SET) >= 0 && read(fd, &c, 1) == 1 && c == '\n');
    close(fd);
    return ok;
}

/* ================================================================
 * 对外接口
 * ================================================================ */

/**
 * @brief  读取 {base}.manifest 到 ManifestInfo
 * @param  base  const char*   进度文件前缀，不能为空
 * @param  out   ManifestInfo* 输出结构体指针，不能为空
 * @return bool  返回 true 表示读取成功且关键字段（run_id/status/schema_version）齐全
 *
 * @note   单行覆盖语义：重复键后写覆盖先写（正常文件每键仅一行，防御性处理）。
 *         未知键忽略，便于后续版本扩展字段。
 */
bool manifest_load(const char *base, ManifestInfo *out) {
    if (!base || !out) return false;
    char path[1200];
    snprintf(path, sizeof(path), "%s.manifest", base);
    FILE *fp = fopen(path, "r");
    if (!fp) return false;

    memset(out, 0, sizeof(*out));
    bool has_run_id = false, has_status = false, has_schema = false;
    char line[4600];
    while (fgets(line, sizeof(line), fp)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *val = eq + 1;
        val[strcspn(val, "\n")] = 0;

        if (strcmp(line, "run_id") == 0) {
            safe_strcpy(out->run_id, val, sizeof(out->run_id));
            has_run_id = true;
        } else if (strcmp(line, "target_path") == 0) {
            safe_strcpy(out->target_path, val, sizeof(out->target_path));
        } else if (strcmp(line, "schema_version") == 0) {
            out->schema_version = atoi(val);
            has_schema = true;
        } else if (strcmp(line, "tool_version") == 0) {
            safe_strcpy(out->tool_version, val, sizeof(out->tool_version));
        } else if (strcmp(line, "status") == 0) {
            safe_strcpy(out->status, val, sizeof(out->status));
            has_status = true;
        } else if (strcmp(line, "started_at") == 0) {
            out->started_at = (time_t)atol(val);
        } else if (strcmp(line, "finished_at") == 0) {
            out->finished_at = (time_t)atol(val);
        } else if (strcmp(line, "baseline_eligible") == 0) {
            out->baseline_eligible = atoi(val);
        } else if (strcmp(line, "baseline_run_id") == 0) {
            safe_strcpy(out->baseline_run_id, val, sizeof(out->baseline_run_id));
        } else if (strcmp(line, "baseline_completed_at") == 0) {
            out->baseline_completed_at = (time_t)atol(val);
        } else if (strcmp(line, "pbin_schema_version") == 0) {
            out->pbin_schema_version = atoi(val);
        }
    }
    fclose(fp);
    return has_run_id && has_status && has_schema;
}

/**
 * @brief  启动时写 status=Running 的 manifest（原子切换）
 * @param  ctx  AppContext*  应用上下文指针，不能为空
 * @return void
 *
 * @note   每次真实运行启动即覆盖写（含续传/盲信），保证崩溃残留的状态
 *         永远是 Running 而非上次终态——修复 .config 追加写多行 status
 *         被 strstr 误判为 Success 的链式基准漏洞。
 *         首次调用时生成 run_id（<时间戳>-<pid>）并记录在 ctx 中，
 *         manifest_finalize 复用同一 run_id。
 */
void manifest_write_running(AppContext *ctx) {
    if (!ctx || !ctx->cfg.progress_base || ctx->cfg.clean) return;

    if (ctx->run_id[0] == '\0') {
        snprintf(ctx->run_id, sizeof(ctx->run_id), "%ld-%d",
                 (long)time(NULL), (int)getpid());
    }

    char buf[4096];
    snprintf(buf, sizeof(buf),
             "run_id=%s\n"
             "target_path=%s\n"
             "schema_version=%d\n"
             "tool_version=%s\n"
             "status=Running\n"
             "started_at=%ld\n"
             "finished_at=0\n"
             "baseline_eligible=0\n"
             "baseline_run_id=%s\n"
             "baseline_completed_at=%ld\n"
             "pbin_schema_version=%d\n",
             ctx->run_id,
             ctx->cfg.target_path ? ctx->cfg.target_path : "",
             MANIFEST_SCHEMA_VERSION,
             VERSION,
             (long)time(NULL),
             ctx->baseline_run_id,
             (long)ctx->baseline_completed_at,
             PBIN_SCHEMA_VERSION);
    manifest_write_atomic(ctx->cfg.progress_base, buf);
}

/**
 * @brief  收尾时写终态 manifest（原子切换），计算 baseline_eligible
 * @param  ctx                   AppContext*  应用上下文指针，不能为空
 * @param  dspill_residue_bytes  long         dspill 未回填残留字节数（0 = 已排空）
 * @param  archive_ok            bool         archive 原子切换校验结果（未启用传 true）
 * @return void
 *
 * @note   baseline_eligible=1 的严格条件（同时满足，任一不满足即为 0）：
 *         1. 本轮非盲信运行（盲信结果不能链式作为基准，设计 §0.4/P0-008）；
 *         2. status == Success（无 skipped/熔断/设备错误）；
 *         3. spbin 残留为 0（compaction 后仍非空 = 有永久跳过目录）；
 *         4. dspill 已排空（残留字节为 0）；
 *         5. 无 fpbin/dfpbin 残留（恢复工作区已闭环）；
 *         6. archive（若启用）写出校验通过；
 *         7. 输出尾部完整（最后一字节为 '\n'）。
 */
void manifest_finalize(AppContext *ctx, long dspill_residue_bytes, bool archive_ok) {
    if (!ctx || !ctx->cfg.progress_base || ctx->cfg.clean) return;
    Config *cfg = &ctx->cfg;
    RuntimeState *state = &ctx->state;

    if (ctx->run_id[0] == '\0') {
        snprintf(ctx->run_id, sizeof(ctx->run_id), "%ld-%d",
                 (long)time(NULL), (int)getpid());
    }

    unsigned long output_offset = 0;
    bool tail_ok = output_tail_complete(cfg, state, &output_offset);

    /* 本次运行的完整性（与盲信身份无关）：
     * 有 skipped/熔断/设备错误、spbin 残留（compaction 后仍非空 = 永久跳过目录）、
     * dspill 未排空、fpbin/dfpbin 残留（恢复工作区未闭环）、archive 校验失败、
     * 输出尾部不完整——任一命中即部分完成，置 has_error 保证退出码为 1。 */
    bool complete = !state->has_error
                    && ctx->spbin_count == 0
                    && dspill_residue_bytes == 0
                    && !fpbin_pair_residue_exists(cfg->progress_base)
                    && archive_ok
                    && tail_ok;
    if (!complete) state->has_error = true;
    const char *status = state->has_error ? "Incomplete" : "Success";

    /* baseline_eligible=1 的严格条件：运行完整 + 非盲信运行
     * （盲信结果不能链式作为基准，设计 §0.4/P0-008）。 */
    bool eligible = complete && !ctx->blind_trust;

    char buf[4096];
    snprintf(buf, sizeof(buf),
             "run_id=%s\n"
             "target_path=%s\n"
             "schema_version=%d\n"
             "tool_version=%s\n"
             "status=%s\n"
             "started_at=%ld\n"
             "finished_at=%ld\n"
             "dirs=%lu\n"
             "files=%lu\n"
             "skipped=%lu\n"
             "errors=%d\n"
             "spbin_count=%lu\n"
             "dspill_residue_bytes=%ld\n"
             "baseline_eligible=%d\n"
             "baseline_run_id=%s\n"
             "baseline_completed_at=%ld\n"
             "output_offset=%lu\n"
             "output_line_count=%lu\n"
             "pbin_schema_version=%d\n",
             ctx->run_id,
             cfg->target_path ? cfg->target_path : "",
             MANIFEST_SCHEMA_VERSION,
             VERSION,
             status,
             (long)state->start_time,
             (long)time(NULL),
             state->dir_count,
             state->file_count,
             state->skipped_count,
             state->has_error ? 1 : 0,
             (unsigned long)ctx->spbin_count,
             dspill_residue_bytes,
             eligible ? 1 : 0,
             ctx->baseline_run_id,
             (long)ctx->baseline_completed_at,
             output_offset,
             state->output_line_count,
             PBIN_SCHEMA_VERSION);
    manifest_write_atomic(cfg->progress_base, buf);
}

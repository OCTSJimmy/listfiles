/**
 * @file main.c
 * @brief listfiles 程序主入口
 *
 * 负责全局初始化、命令行解析、资源分配、工作流编排与最终清理。
 * 主要流程：初始化 AppContext → 解析参数 → 加载历史配置 → 确认任务 →
 * 创建设备管理器/输出句柄/异步写线程/Worker 池/探测调度器/监控线程 →
 * 恢复进度（如有）→ 发送根任务 → 运行 epoll 主循环 → 停止监控 → 归档清理 → 释放资源。
 */
#define _GNU_SOURCE
#include "config.h"
#include "cmdline.h"
#include "app_context.h"
#include "main_loop.h"
#include "output.h"
#include "progress.h"
#include "manifest.h"
#include "utils.h"
#include "signals.h"
#include "log.h"
#include "msg_format.h"
#include "msg_queue.h"
#include "circuit_breaker.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <stdatomic.h>
#include <sys/sysinfo.h>
#include <errno.h>

/**
 * @brief  初始化 AppContext 结构体
 * @param  ctx  AppContext*  指向要初始化的应用上下文指针，不能为空
 * @return void
 *
 * @note   将结构体内存清零，设置 epfd 和 event_fd 为 -1，
 *         初始化原子计数器 pending_tasks/pending_batches 为 0，
 *         初始化 record_batch 批量缓冲。
 */
static void app_context_init(AppContext *ctx) {
    memset(ctx, 0, sizeof(AppContext));
    ctx->epfd = -1;
    ctx->event_fd = -1;
    ctx->running = false;
    ctx->hist_pump_state = HIST_PUMP_DONE;
    ctx->next_requeue_worker = 0;
    atomic_init(&ctx->pending_tasks, 0);
    atomic_init(&ctx->pending_batches, 0);
    atomic_init(&ctx->epoch_counter, 0);  /* v15.6.0: epoch 从 1 开始（自增后生效），0 表示无在途任务 */
    dispatch_queue_init(&ctx->dispatch_queue);
    record_path_batch_init(&ctx->record_batch);
    pthread_mutex_init(&ctx->dspill_mutex, NULL); /* v15.5.8 */
    /* v15.5.9: 初始化 redispatch 退避数组 */
    for (int i = 0; i < MAX_WORKERS; i++) {
        ctx->redispatch_backoff_until[i] = 0;
    }
}

/**
 * @brief  销毁 AppContext 并释放所有内部资源
 * @param  ctx  AppContext*  指向应用上下文的指针，不能为空
 * @return void
 *
 * @note   按依赖反序释放：刷出 record_batch → 销毁线程池 → 关闭 eventfd →
 *         关闭异步写线程 → 销毁 Worker 池 → 销毁探测调度器 → 销毁设备管理器 →
 *         销毁指纹集合 → 释放 spbin 缓存 → 关闭 fpbin 文件 → 释放 fpbin 内存数组。
 *         每个指针释放后均置为 NULL，防止重复释放。
 */
static void app_context_destroy(AppContext *ctx) {
    /* 刷出残留的 record_path 缓冲 */
    if (ctx->cfg.progress_base) {
        record_path_batch_flush(&ctx->cfg, &ctx->state, &ctx->record_batch);
    }
    if (ctx->thread_pool) {
        thread_pool_destroy(ctx->thread_pool);
        ctx->thread_pool = NULL;
    }
    if (ctx->event_fd >= 0) {
        close(ctx->event_fd);
        ctx->event_fd = -1;
    }
    if (ctx->async_writer) {
        async_worker_shutdown(ctx->async_writer);
        ctx->async_writer = NULL;
    }
    free(ctx->output_pending);   /* v15.6.0: P0-002 输出三态计数数组 */
    ctx->output_pending = NULL;
    if (ctx->worker_pool) {
        worker_pool_destroy(ctx->worker_pool);
        ctx->worker_pool = NULL;
    }
    if (ctx->probe_scheduler) {
        probe_scheduler_destroy(ctx->probe_scheduler);
        ctx->probe_scheduler = NULL;
    }
    if (ctx->dev_mgr) {
        dev_mgr_destroy(ctx->dev_mgr);
        ctx->dev_mgr = NULL;
    }
    circuit_breaker_close(ctx);
    if (ctx->discovered_set) {
        fp_set_destroy(ctx->discovered_set);
        ctx->discovered_set = NULL;
    }
    if (ctx->enqueued_set) {
        fp_set_destroy(ctx->enqueued_set);
        ctx->enqueued_set = NULL;
    }
    if (ctx->completed_set) {
        fp_set_destroy(ctx->completed_set);
        ctx->completed_set = NULL;
    }
    dispatch_queue_destroy(&ctx->dispatch_queue);
    if (ctx->reference_set) {
        fp_set_destroy(ctx->reference_set);
        ctx->reference_set = NULL;
    }
    if (ctx->reference_map) {
        ref_map_destroy(ctx->reference_map);
        ctx->reference_map = NULL;
    }
    if (ctx->spbin_entries) {
        for (size_t i = 0; i < ctx->spbin_count; i++) {
            free(ctx->spbin_entries[i].path);
        }
        free(ctx->spbin_entries);
        ctx->spbin_entries = NULL;
    }
    /* v15.6.0（P0-005）：spbin 路径指纹集合 */
    if (ctx->spbin_set) {
        fp_set_destroy(ctx->spbin_set);
        ctx->spbin_set = NULL;
    }
    /* v15.6.0（P1-004）：毒丸目录致死计数 */
    if (ctx->poison_paths) {
        for (size_t i = 0; i < ctx->poison_count; i++) {
            free(ctx->poison_paths[i]);
        }
        free(ctx->poison_paths);
        ctx->poison_paths = NULL;
    }
    free(ctx->poison_counts);
    ctx->poison_counts = NULL;
    if (ctx->fpbin_slice_file) {
        fclose(ctx->fpbin_slice_file);
        ctx->fpbin_slice_file = NULL;
    }
    /* v15.6.0: 关闭 dfpbin 原子对句柄（P0-003） */
    if (ctx->dfpbin_slice_file) {
        fclose(ctx->dfpbin_slice_file);
        ctx->dfpbin_slice_file = NULL;
    }
    /* v15.5.8: 关闭 dspill 派发兜底文件句柄 */
    if (ctx->dspill_fp) {
        fclose(ctx->dspill_fp);
        ctx->dspill_fp = NULL;
    }
    pthread_mutex_destroy(&ctx->dspill_mutex);
    if (ctx->fpbin_entries) {
        for (size_t i = 0; i < ctx->fpbin_count; i++) {
            free(ctx->fpbin_entries[i]);
        }
        free(ctx->fpbin_entries);
        ctx->fpbin_entries = NULL;
    }
    if (ctx->fpbin_stats) {
        free(ctx->fpbin_stats);
        ctx->fpbin_stats = NULL;
    }
}

/**
 * @brief  检查 progress 前缀路径是否存在任何进度文件残留（v15.6.0，P0-008）
 * @param  base  const char*  进度文件前缀，不能为空
 * @return bool  存在 .config/.manifest/pbin 分片/archive 任一残留返回 true
 *
 * @note   盲信扫描要求新的空 progress 目录（设计 §4.3）：-f 路径有残留即拒绝。
 */
static bool progress_base_has_residue(const char *base) {
    char path[1024];
    snprintf(path, sizeof(path), "%s.config", base);
    if (access(path, F_OK) == 0) return true;
    snprintf(path, sizeof(path), "%s.manifest", base);
    if (access(path, F_OK) == 0) return true;
    char *slice = get_slice_filename(base, 0);
    bool found = (access(slice, F_OK) == 0);
    free(slice);
    char *arch = get_archive_filename(base);
    if (access(arch, F_OK) == 0) found = true;
    free(arch);
    return found;
}

/**
 * @brief  加载会话配置快照并进行一致性校验（v15.6.0：以 manifest 为准）
 * @param  cfg         Config*  指向当前配置结构体的指针，不能为空
 * @param  has_history bool*   输出参数，返回 true 表示检测到历史进度文件
 * @return void
 *
 * @note   v15.6.0（P0-008）：权威状态来源从 .config 切换为 {base}.manifest——
 *         旧实现中 finalize 向 .config 追加写积累多行 status，逐行/strstr 匹配
 *         会命中任意一行（如 Running 后追加 Incomplete 仍被当 Running），
 *         manifest 为原子写、单行覆盖语义，根除该误判。
 *         .config 继续按原样写入（兼容性），此处仅读其中的 archive 策略字段。
 *         兼容性策略（不做双格式解析）：manifest 缺失（旧版本进度）或
 *         schema_version/pbin_schema_version 与当前（2/2）不一致 → 拒绝续传，
 *         exit(2)，提示 --runone 重新全量扫描。
 *         path 不一致、archive 策略不一致同样 exit(2)（进度不兼容）。
 */
static void load_session_config(Config *cfg, bool *has_history) {
    *has_history = false;
    if (!cfg->progress_base) return;
    char config_path[1024];
    snprintf(config_path, sizeof(config_path), "%s.config", cfg->progress_base);
    bool config_exists = (access(config_path, F_OK) == 0);

    ManifestInfo m;
    bool m_ok = manifest_load(cfg->progress_base, &m);
    if (!config_exists && !m_ok) return;
    *has_history = true;

    if (!m_ok) {
        log_error("进度文件为旧版本格式（缺少 %s.manifest），无法安全续传/盲信。", cfg->progress_base);
        log_error("请使用 --runone 重新全量扫描。");
        exit(2);
    }
    if (m.schema_version != MANIFEST_SCHEMA_VERSION
        || m.pbin_schema_version != PBIN_SCHEMA_VERSION) {
        log_error("进度文件格式不兼容（manifest schema=%d, pbin schema=%d，当前要求 %d/%d）。",
                  m.schema_version, m.pbin_schema_version,
                  MANIFEST_SCHEMA_VERSION, PBIN_SCHEMA_VERSION);
        log_error("请使用 --runone 重新全量扫描。");
        exit(2);
    }
    if (m.target_path[0] != '\0' && strcmp(cfg->target_path, m.target_path) != 0) {
        log_error("检测到进度文件与当前路径不一致！");
        log_error("  历史记录: %s", m.target_path);
        log_error("  当前指定: %s", cfg->target_path);
        log_error("建议：使用 --runone 强制重跑，或检查 --progress-file 参数。");
        exit(2);
    }
    if (strcmp(m.status, "Success") == 0 || strcmp(m.status, "Running") == 0
        || strcmp(m.status, "Incomplete") == 0) {
        cfg->continue_mode = true;
    }

    /* archive 策略一致性仍读 .config（manifest 不收录该字段） */
    if (config_exists) {
        FILE *fp = fopen(config_path, "r");
        if (fp) {
            char line[1024];
            while (fgets(line, sizeof(line), fp)) {
                char *eq = strchr(line, '=');
                if (!eq) continue;
                *eq = '\0';
                char *val = eq + 1;
                val[strcspn(val, "\n")] = 0;
                if (strcmp(line, "archive") == 0) {
                    bool hist_archive = atoi(val);
                    if (hist_archive != cfg->archive) {
                        log_error("归档策略与历史记录不一致");
                        log_error("请使用 --runone 重新全量扫描。");
                        exit(2);
                    }
                    break; /* archive 由 save_config_to_disk 单次写入，首行即为有效值 */
                }
            }
            fclose(fp);
        }
    }
}

/**
 * @brief  交互式任务确认（仅在未指定 --yes 时执行）
 * @param  cfg          const Config*  指向当前配置的只读指针，不能为空
 * @param  has_history  bool           是否检测到历史进度（影响模式显示）
 * @return void
 *
 * @note   向 stdout 打印任务概要（目标路径、运行模式、输出格式、半增量阈值、batch 大小），
 *         等待用户输入 'Y' 或 'y' 确认；其他输入则 exit(0) 取消任务。
 */
static void interactive_confirm(const Config *cfg, bool has_history) {
    if (cfg->sure) return;
    printf("\n=== 任务确认 ===\n");
    printf("目标路径: %s\n", cfg->target_path);
    if (cfg->runone) {
        printf("运行模式: 强制全量\n");
    } else if (has_history && cfg->continue_mode) {
        printf("运行模式: 智能续传/增量\n");
    } else {
        printf("运行模式: 全量扫描\n");
    }
    if (cfg->csv) printf("输出格式: CSV\n");
    printf("半增量阈值: %ld 秒\n", cfg->skip_interval);
    printf("Worker batch: %d\n", cfg->batch_size);
    printf("\n按 [Y] 继续，其他键退出: ");
    char c = getchar();
    if (c != 'y' && c != 'Y') {
        printf("已取消。\n");
        exit(0);
    }
}

/**
 * @brief  初始化输出流的缓冲区大小
 * @param  ctx  AppContext*  指向应用上下文的指针，不能为空
 * @return void
 *
 * @note   主数据输出流设置 8MB 全缓冲；目录信息输出流设置 1MB 全缓冲。
 *         仅在输出流不是 stdout/stderr 时生效，用于减少 fwrite 系统调用次数。
 */
static void init_output_buffers(AppContext *ctx) {
    if (ctx->state.output_fp && ctx->state.output_fp != stdout) {
        setvbuf(ctx->state.output_fp, NULL, _IOFBF, 8 * 1024 * 1024);
    }
    if (ctx->state.dir_info_fp && ctx->state.dir_info_fp != stderr) {
        setvbuf(ctx->state.dir_info_fp, NULL, _IOFBF, 1 * 1024 * 1024);
    }
}

/**
 * @brief  listfiles 程序入口
 * @param  argc  int     命令行参数个数
 * @param  argv  char**  命令行参数字符串数组
 * @return int   返回 0 表示任务成功完成；返回 1 表示发生错误
 *
 * @note   完整流程参见文件头部注释。关键设计点：
 *         - 使用 fork() + pipe 的 Worker 进程模型，通过 COW 共享只读上下文。
 *         - 半增量模式（skip_interval > 0）下加载 reference_set/map。
 *         - 单文件目标直接提交到 async_writer，不创建 Worker 任务。
 *         - 主循环退出后先 join 监控线程，再执行 finalize_progress 归档。
 */
int main(int argc, char *argv[]) {
    AppContext ctx;
    app_context_init(&ctx);

    init_config(&ctx.cfg);
    int parse_ret = parse_arguments(argc, argv, &ctx.cfg);
    if (parse_ret == 2) {
        /* --help or --version */
        app_context_destroy(&ctx);
        free(ctx.cfg.progress_base);
        return 0;
    }
    if (parse_ret != 0) {
        return 1;
    }
    setup_signal_handlers();

    bool has_history = false;
    if (ctx.cfg.runone || ctx.cfg.clean) {
        RuntimeState temp = {0};
        cleanup_progress(&ctx.cfg, &temp);
        ctx.cfg.continue_mode = false;
    }
    if (!ctx.cfg.runone) {
        load_session_config(&ctx.cfg, &has_history);
    }

    /* v15.6.0（P0-008/P0-011）：盲信扫描启动校验（必须先于 manifest_write_running
     * 覆盖写 Running，否则基准 manifest 被本轮覆盖、baseline_eligible 无法判定）。
     * 盲信 = 续传 + skip_interval>0 + 非 runone；规则：
     * 1. 必须显式 --reference-base 指定基准（旧 progress 路径）；
     * 2. -f 路径必须无进度文件残留（新空 progress 目录，增量与基准物理隔离）；
     * 3. 基准 manifest 存在且 baseline_eligible=1、pbin_schema_version=2。
     * 任一不满足 → 拒绝运行 exit(2)（严重失败：盲信基准不合格）。 */
    ctx.blind_trust = ctx.cfg.continue_mode && !ctx.cfg.runone && ctx.cfg.skip_interval > 0;
    if (ctx.blind_trust) {
        if (!ctx.cfg.reference_base) {
            log_error("盲信扫描（--skip-interval>0）须用 --reference-base 显式指定盲信基准（旧 progress 路径）。");
            exit(2);
        }
        if (progress_base_has_residue(ctx.cfg.progress_base)) {
            log_error("盲信扫描要求新的空 progress 目录：%s 已有进度文件残留。", ctx.cfg.progress_base);
            log_error("请为 -f/--progress-file 指定全新路径（基准经 --reference-base 指定）。");
            exit(2);
        }
        ManifestInfo base_m;
        if (!manifest_load(ctx.cfg.reference_base, &base_m)) {
            log_error("盲信基准 %s 缺少 manifest（旧版本进度或不完整），拒绝盲信扫描。",
                      ctx.cfg.reference_base);
            log_error("请先用当前版本对目标做全量扫描建立合格基准，或使用 --runone 重新全量扫描。");
            exit(2);
        }
        if (base_m.pbin_schema_version != PBIN_SCHEMA_VERSION || base_m.baseline_eligible != 1) {
            log_error("盲信基准不合格（%s：baseline_eligible=%d, pbin_schema_version=%d），拒绝盲信扫描。",
                      ctx.cfg.reference_base, base_m.baseline_eligible, base_m.pbin_schema_version);
            log_error("请重新全量扫描建立合格基准（status=Success 且无 spbin/dspill/fpbin 残留）。");
            exit(2);
        }
        safe_strcpy(ctx.baseline_run_id, base_m.run_id, sizeof(ctx.baseline_run_id));
        ctx.baseline_completed_at = base_m.finished_at;
        log_info("盲信基准校验通过：%s（run_id=%s）", ctx.cfg.reference_base, base_m.run_id);
    }

    interactive_confirm(&ctx.cfg, has_history);

    /* v15.6.0（P0-008）：续传兼容性兜底——有 pbin/archive 二进制残留但无
     * .config/.manifest（旧版本进度）→ 拒绝续传，不做双格式解析。
     * （.config/.manifest 存在的情形已由 load_session_config 校验并拦截） */
    if (ctx.cfg.continue_mode && !ctx.blind_trust && !has_history
        && progress_base_has_residue(ctx.cfg.progress_base)) {
        log_error("检测到旧版本进度残留（无 manifest），无法安全续传。");
        log_error("请使用 --runone 重新全量扫描。");
        exit(2);
    }

    /* Initialize logging with verbose settings */
    log_init(ctx.cfg.verbose, ctx.cfg.verbose_level);

    /* Set version threshold: default = VERSION_CODE, override if --verbose-version specified */
    if (ctx.cfg.verbose_version != ULONG_MAX) {
        log_set_version_threshold(ctx.cfg.verbose_version);
    } else {
        log_set_version_threshold(VERSION_CODE);
    }

    ctx.state.start_time = time(NULL);
    ctx.state.has_error = false;
    ctx.dev_mgr = dev_mgr_create();

    if (!ctx.cfg.continue_mode || ctx.cfg.runone || !has_history) {
        save_config_to_disk(&ctx.cfg);
    }
    /* v15.6.0（P0-008）：启动即原子写 status=Running 的 Run manifest
     * （.new → fsync → rename），覆盖上次运行终态——崩溃残留的状态永远是
     * Running 而非 Success，修复 .config 追加写多行 status 被误判的链式基准漏洞。
     * manifest 是唯一权威状态来源，.config 仅为兼容保留。 */
    manifest_write_running(&ctx);

    /* v15.6.1（P0-107a）：fork 时序前移——全部 fork（初始 Worker + 预备役）集中在
     * 单线程期、巨型指纹集合分配之前、一切 pthread_create 之前。刚性约束（生产事故
     * R7/R8，见 Design-todo-v15.6.1.md）：
     * 1. 严格超售（vm.overcommit_memory=2）下 fork 按全额 VSZ 计 commit，
     *    estimated-files 预分配把 VSZ 吹大后运行期 fork 必败（ENOMEM，54h 静默）；
     * 2. 多线程进程 fork 的子进程继承锁状态，可能永久死锁。
     * 盲信模式的基准索引（reference_set/map）是 Worker 的 COW 只读上下文，必须在
     * fork 前加载——盲信场景不在本优化覆盖范围（该功能已整体押后）。 */

    /* v15.6.0（P0-011）：盲信模式加载基准索引（启动校验已在前置关卡完成）。
     * 基准经 --reference-base 显式指定，与本轮新空 progress 目录物理隔离；
     * reference_set/map 以纯路径指纹为 key，收录基准完整历史 stat。 */
    if (ctx.blind_trust) {
        log_info("加载盲信基准索引进行盲信扫描...");
        ctx.reference_set = fp_set_create(ctx.cfg.estimated_files);
        ctx.reference_map = ref_map_create(ctx.cfg.estimated_files);
        if (!ctx.reference_set || !ctx.reference_map) {
            log_fatal("无法分配盲信基准索引内存");
            app_context_destroy(&ctx);
            return 2;
        }
        restore_progress_to_memory(&ctx.cfg, &ctx, ctx.cfg.reference_base);
        log_info("历史索引加载完成");
    }

    /* Setup worker context (COW, read-only in workers) */
    worker_set_context(&ctx.cfg, ctx.reference_set, ctx.reference_map);

    /* Create worker pool */
    int num_workers = ctx.cfg.worker_count;
    if (num_workers <= 0) {
        int num_cores = get_nprocs();
        if (num_cores < 1) num_cores = 4;
        num_workers = num_cores * 2;
        if (num_workers > 8) num_workers = 8;  // 默认上限 8，防止 NFS 过载
    }
    ctx.worker_pool = worker_pool_create(num_workers);
    /* v15.6.0: 输出三态（P0-002）——每 slot 未 COMMITTED 输出 batch 计数，按 num_workers 动态分配 */
    ctx.output_pending = calloc((size_t)num_workers, sizeof(_Atomic long));
    ctx.probe_scheduler = probe_scheduler_create();

    if (!ctx.worker_pool || !ctx.output_pending || !ctx.probe_scheduler) {
        log_fatal("无法初始化进程池");
        app_context_destroy(&ctx);
        return 2; /* v15.6.0: 严重失败 */
    }

    /* v15.6.1（P0-107a/P0-108）：单线程期 fork 全部初始 Worker + 预备役 spare，
     * 此后运行期零 fork。单个初始 fork 失败：slot 置 pid=-1，交由运行期替换环路
     * 用 spare 补位自愈；全部失败则致命。 */
    int spawned = 0;
    for (int i = 0; i < num_workers; i++) {
        if (worker_pool_spawn(ctx.worker_pool, i)) {
            spawned++;
        } else {
            ctx.worker_pool->slots[i].pid = -1;  /* 交给运行期 spare 补位 */
        }
    }
    if (spawned == 0) {
        log_fatal("无法 fork 任何 Worker（原因见上方 errno），终止运行");
        app_context_destroy(&ctx);
        return 2;
    }
    if (spawned < num_workers) {
        log_error("仅 %d/%d 个 Worker fork 成功，降额启动", spawned, num_workers);
    }
    worker_pool_spawn_spares(ctx.worker_pool, num_workers);

    /* Pre-allocate fingerprint sets (v15.6.0 P0-004: discovered/enqueued 拆分)
     * v15.6.1：移到全部 fork 之后——缩小 fork 时 VSZ（严格超售下 fork 按全额 VSZ
     * 计 commit）；这些集合仅 Master 侧使用，Worker 不访问。 */
    ctx.discovered_set = fp_set_create(ctx.cfg.estimated_files);
    if (!ctx.discovered_set) {
        log_fatal("无法分配 DiscoveredSet 内存");
        return 2; /* v15.6.0: 严重失败 */
    }
    ctx.enqueued_set = fp_set_create(ctx.cfg.estimated_files);
    if (!ctx.enqueued_set) {
        log_fatal("无法分配 EnqueuedSet 内存");
        return 2; /* v15.6.0: 严重失败 */
    }

    /* v15.6.1（P0-107a）：monitor 创建与线程启动必须在全部 fork 之后 */
    ctx.monitor = monitor_create(&ctx);
    if (!ctx.monitor) {
        log_fatal("无法初始化进程池");
        app_context_destroy(&ctx);
        return 2;
    }
    pthread_create(&ctx.monitor->tid, NULL, monitor_thread_entry, ctx.monitor);

    /* v13.0.0: Initialize IPC threads and send initial REPLACE */
    if (!init_ipc_threads(&ctx)) {
        log_fatal("IPC thread initialization failed");
        app_context_destroy(&ctx);
        return 2; /* v15.6.0: 严重失败 */
    }
    for (int i = 0; i < num_workers; i++) {
        WorkerSlot *slot = &ctx.worker_pool->slots[i];
        /* v15.6.1：启动 fork 失败的 slot（pid=-1）留给运行期 spare 补位，
         * 不得把未初始化的 fd 发给 IPC 线程 */
        if (!atomic_load(&slot->is_alive)) continue;
        send_replace_to_ipc(&ctx, i, slot->fd_cmd, slot->fd_data, slot->fd_ctrl, slot->pid);
    }

    /* Resume mode: restore progress and replay unfinished tasks */
    if (ctx.cfg.continue_mode && !ctx.reference_set) {
        restore_progress(&ctx.cfg, &ctx);
    }

    /* [FIX] 必须在 restore_progress 之后初始化输出文件，否则 output_slice_num 等状态会被覆盖 */
    init_output_files(&ctx.cfg, &ctx.state);
    init_output_buffers(&ctx);
    ctx.async_writer = async_worker_init(&ctx.cfg, &ctx.state, ctx.output_pending, &ctx.main_cond);
    circuit_breaker_init(&ctx);

    /* v15.6.0（P0-004/P1-001）：dspill 已纳入恢复——restore_progress 会读取
     * 上一运行遗留的 dspill 兜底文件并经 enqueue_dir 回填（enqueued_set 去重），
     * 不再在启动时无条件删除。强制重跑（--runone/--clean）由 cleanup_progress 清理。 */

    /* Seed root task */
    struct stat root_info;
    if (lstat(ctx.cfg.target_path, &root_info) == 0) {
        ctx.state.root_dev = root_info.st_dev;  /* v15.5.6: 单挂载保护基准 */
        if (S_ISDIR(root_info.st_mode)) {
            /* v15.6.0: 根任务统一经 enqueue_dir → dispatch_queue →
             * dispatch_from_queue 派发，由派发路径负责 pending_tasks++、epoch
             * 分配、Worker BUSY 状态与 dpbin。
             * 此前旁路直发 CMD_SCAN：不占 BUSY 态，READY 后 slot 被判 IDLE，
             * dispatch 会把第二个任务叠上同一 Worker，且 epoch 未登记，
             * 根任务的 BATCH/FINISH 被 epoch 校验全部误杀 → pending_tasks 泄漏卡死
             * （回归用例7 dspill 场景复现）。
             * v15.6.0（P0-004）：改走统一入队入口后，恢复场景下根任务若已在
             * completed_set 则被剪枝（其子目录由 pbin 泵送覆盖），若泵送差集
             * 也含根目录则 enqueued_set 防重，根目录只会入队一次。 */
            enqueue_dir(&ctx, ctx.cfg.target_path, &root_info);
            /* 防御：根任务必须已入队、已落 dspill 或已被 completed_set 剪枝，
             * 否则扫描将空跑（原直推路径此处为 log_fatal） */
            if (dispatch_queue_count(&ctx.dispatch_queue) == 0 && !ctx.dspill_fp) {
                uint8_t root_fp[FP_SIZE];
                fp_compute(ctx.cfg.target_path, root_info.st_dev, root_info.st_ino, root_fp);
                if (!ctx.completed_set || !fp_set_contains(ctx.completed_set, root_fp)) {
                    log_fatal("根任务入队失败");
                    app_context_destroy(&ctx);
                    return 2; /* v15.6.0: 严重失败 */
                }
            }
        } else {
            /* Single file target */
            async_writer_submit(ctx.async_writer, ctx.cfg.target_path, &root_info);
            ctx.state.file_count++;
        }
    } else {
        log_fatal("Cannot access target path %s", ctx.cfg.target_path);
        app_context_destroy(&ctx);
        return 2; /* v15.6.0: 严重失败 */
    }

    if (!ctx.cfg.mute) {
        log_info("任务开始...");
    }
    main_loop_run(&ctx);

    /* Stop monitor thread after main loop exits */
    if (ctx.monitor) {
        ctx.monitor->running = false;
        pthread_join(ctx.monitor->tid, NULL);
    }

    if (!ctx.cfg.mute) {
        log_info("任务完成。耗时: %ld 秒", time(NULL) - ctx.state.start_time);
    }

    /* v15.5.8: dspill 兜底统计（>0 说明曾发生 HIGH_WATER 跳推，已全部回填） */
    if (ctx.dspill_appended > 0) {
        log_info("[Dspill] HIGH_WATER 跳推目录 %lu 个，回填 %lu 个",
                 ctx.dspill_appended, ctx.dspill_loaded);
    }

    if (ctx.state.skipped_count > 0) {
        fprintf(stderr,
                "[CRITICAL] 扫描不完整：已跳过 %lu 个路径。详见 %s.circuit_breaker\n",
                ctx.state.skipped_count, ctx.cfg.progress_base);
        ctx.state.has_error = true;
    }

    /* v15.6.0（P0-005）：正常退出时 spbin compaction——过滤已恢复（RECOVERED）
     * 条目后重写 {base}.spbin，须先于 finalize_progress（archive 模式会把
     * spbin 归档进 {base}.archive，此处保证归档的是压缩后的版本） */
    spbin_compact(&ctx);

    /* v15.6.0（P0-008）：先关停异步输出线程并刷盘，保证 manifest 的
     * output_offset/输出尾部完整性校验基于落盘后的最终内容 */
    if (ctx.async_writer) {
        async_worker_shutdown(ctx.async_writer);
        ctx.async_writer = NULL;
    }
    if (ctx.state.output_fp && ctx.state.output_fp != stdout) {
        fflush(ctx.state.output_fp);
    }

    /* v15.6.0（P0-008）：dspill 残留字节统计（manifest 字段 + baseline_eligible
     * 判定条件），须先于成功路径的 dspill 删除 */
    long dspill_residue = 0;
    if (ctx.cfg.progress_base) {
        char *spill = get_dspill_filename(ctx.cfg.progress_base);
        struct stat spill_st;
        if (spill && stat(spill, &spill_st) == 0
            && spill_st.st_size > (off_t)ctx.dspill_read_offset) {
            dspill_residue = (long)(spill_st.st_size - (off_t)ctx.dspill_read_offset);
        }
        free(spill);
    }

    bool archive_ok = finalize_progress(&ctx.cfg, &ctx.state);
    /* v15.6.0（P0-008）：写终态 Run manifest（.new → fsync → rename 原子切换），
     * 含 status/baseline_eligible 判定；部分完成时 manifest_finalize 会置
     * has_error，保证退出码为 1（部分完成） */
    manifest_finalize(&ctx, dspill_residue, archive_ok);
    /* v15.5.0: Delete temporary dpbin after successful completion */
    if (ctx.cfg.continue_mode && ctx.cfg.progress_base) {
        dpbin_delete_all(ctx.cfg.progress_base);
    }
    /* v15.5.8: 成功完结后删除 dspill 兜底文件；有错误时保留供审计 */
    if (ctx.cfg.progress_base && !ctx.state.has_error) {
        char *spill = get_dspill_filename(ctx.cfg.progress_base);
        if (spill) {
            unlink(spill);
            free(spill);
        }
    }
    app_context_destroy(&ctx);

    /* 释放命令行参数分配的字符串内存 */
    free(ctx.cfg.target_path);
    free(ctx.cfg.output_file);
    free(ctx.cfg.output_split_dir);
    free(ctx.cfg.progress_base);
    free(ctx.cfg.reference_base); /* v15.6.0: --reference-base */
    free(ctx.cfg.format);
    free(ctx.cfg.resume_file);

    /* v15.6.0 退出码语义（§9.6）：0=完全完成；1=部分完成（跳过/spbin 残留/
     * dspill 残留/熔断清单非空）；2=严重失败（盲信基准不合格、进度不兼容、
     * 初始化致命错误）；3=架构不匹配（预留，本期不接平台检查） */
    return ctx.state.has_error ? 1 : 0;
}

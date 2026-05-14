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
#include "utils.h"
#include "signals.h"
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
    pthread_mutex_init(&ctx->lost_tasks_mutex, NULL);
    record_path_batch_init(&ctx->record_batch);
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
    if (ctx->visited_set) {
        fp_set_destroy(ctx->visited_set);
        ctx->visited_set = NULL;
    }
    pthread_mutex_destroy(&ctx->lost_tasks_mutex);
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
    if (ctx->fpbin_slice_file) {
        fclose(ctx->fpbin_slice_file);
        ctx->fpbin_slice_file = NULL;
    }
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
 * @brief  加载会话配置快照并进行一致性校验
 * @param  cfg         Config*  指向当前配置结构体的指针，不能为空
 * @param  has_history bool*   输出参数，返回 true 表示检测到历史进度文件(.config)
 * @return void
 *
 * @note   读取 {progress_base}.config 文件，校验 path 字段是否与当前 --path 一致。
 *         若不一致则打印错误并 exit(1)。根据 status 字段自动设置 continue_mode。
 *         若 archive 策略与历史记录不一致也直接 exit(1)。
 */
static void load_session_config(Config *cfg, bool *has_history) {
    *has_history = false;
    if (!cfg->progress_base) return;
    char path[1024];
    snprintf(path, sizeof(path), "%s.config", cfg->progress_base);
    if (access(path, F_OK) != 0) return;
    *has_history = true;

    FILE *fp = fopen(path, "r");
    if (!fp) return;

    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = line;
        char *val = eq + 1;
        val[strcspn(val, "\n")] = 0;

        if (strcmp(key, "path") == 0) {
            if (strcmp(cfg->target_path, val) != 0) {
                fprintf(stderr, "\n[错误] 检测到进度文件与当前路径不一致！\n");
                fprintf(stderr, "  历史记录: %s\n", val);
                fprintf(stderr, "  当前指定: %s\n", cfg->target_path);
                fprintf(stderr, "建议：使用 --runone 强制重跑，或检查 --progress-file 参数。\n");
                exit(1);
            }
        } else if (strcmp(key, "status") == 0) {
            if (strcmp(val, "Success") == 0 || strcmp(val, "Running") == 0) {
                cfg->continue_mode = true;
            }
        } else if (strcmp(key, "archive") == 0) {
            bool hist_archive = atoi(val);
            if (hist_archive != cfg->archive) {
                fprintf(stderr, "[错误] 归档策略与历史记录不一致\n");
                exit(1);
            }
        }
    }
    fclose(fp);
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
    interactive_confirm(&ctx.cfg, has_history);

    ctx.state.start_time = time(NULL);
    ctx.state.has_error = false;
    ctx.dev_mgr = dev_mgr_create();

    if (!ctx.cfg.continue_mode || ctx.cfg.runone || !has_history) {
        save_config_to_disk(&ctx.cfg);
    }

    /* Pre-allocate fingerprint set */
    ctx.visited_set = fp_set_create(ctx.cfg.estimated_files);
    if (!ctx.visited_set) {
        fprintf(stderr, "[Fatal] 无法分配 VisitedSet 内存\n");
        return 1;
    }

    /* Incremental mode: load reference set/map */
    if (ctx.cfg.continue_mode && ctx.cfg.skip_interval > 0) {
        char path[1024];
        snprintf(path, sizeof(path), "%s.config", ctx.cfg.progress_base);
        FILE *fp = fopen(path, "r");
        bool is_success = false;
        if (fp) {
            char line[1024];
            while (fgets(line, sizeof(line), fp)) {
                if (strstr(line, "status=Success")) {
                    is_success = true;
                    break;
                }
            }
            fclose(fp);
        }
        if (is_success) {
            fprintf(stderr, "[System] 检测到上次任务已完成，加载历史索引进行半增量扫描...\n");
            ctx.reference_set = fp_set_create(ctx.cfg.estimated_files);
            ctx.reference_map = ref_map_create(ctx.cfg.estimated_files);
            restore_progress_to_memory(&ctx.cfg, &ctx);
            fprintf(stderr, "[System] 历史索引加载完成\n");
        }
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
    ctx.probe_scheduler = probe_scheduler_create();
    ctx.monitor = monitor_create(&ctx);

    if (!ctx.worker_pool || !ctx.probe_scheduler || !ctx.monitor) {
        fprintf(stderr, "[Fatal] 无法初始化进程池\n");
        app_context_destroy(&ctx);
        return 1;
    }

    /* Start monitor thread */
    pthread_create(&ctx.monitor->tid, NULL, monitor_thread_entry, ctx.monitor);

    /* Spawn all workers before any restore/replay (needed for resume dispatch) */
    for (int i = 0; i < num_workers; i++) {
        worker_pool_spawn(ctx.worker_pool, i);
    }

    /* Resume mode: restore progress and replay unfinished tasks */
    if (ctx.cfg.continue_mode && !ctx.reference_set) {
        restore_progress(&ctx.cfg, &ctx);
    }

    /* [FIX] 必须在 restore_progress 之后初始化输出文件，否则 output_slice_num 等状态会被覆盖 */
    init_output_files(&ctx.cfg, &ctx.state);
    init_output_buffers(&ctx);
    ctx.async_writer = async_worker_init(&ctx.cfg, &ctx.state);

    /* Seed root task */
    struct stat root_info;
    if (lstat(ctx.cfg.target_path, &root_info) == 0) {
        if (S_ISDIR(root_info.st_mode)) {
            atomic_fetch_add(&ctx.pending_tasks, 1);
            uint32_t plen = (uint32_t)strlen(ctx.cfg.target_path);
            WorkerSlot *slot = ctx.worker_pool->slots;
            slot->current_dev = root_info.st_dev;
            safe_strcpy(slot->current_path, ctx.cfg.target_path, sizeof(slot->current_path));
            int rc = ipc_send(slot->fd_in, IPC_MSG_SCAN, ctx.cfg.target_path, plen);
            if (rc != 0) {
                fprintf(stderr, "[Fatal] 根任务发送失败: worker 0 fd=%d rc=%d errno=%d (%s)\n",
                        slot->fd_in, rc, errno, strerror(errno));
                app_context_destroy(&ctx);
                return 1;
            }
        } else {
            /* Single file target */
            async_writer_submit(ctx.async_writer, ctx.cfg.target_path, &root_info);
            ctx.state.file_count++;
        }
    } else {
        fprintf(stderr, "Fatal: Cannot access target path %s\n", ctx.cfg.target_path);
        app_context_destroy(&ctx);
        return 1;
    }

    if (!ctx.cfg.mute) {
        fprintf(stderr, "[System] 任务开始...\n");
    }
    main_loop_run(&ctx);

    /* Stop monitor thread after main loop exits */
    if (ctx.monitor) {
        ctx.monitor->running = false;
        pthread_join(ctx.monitor->tid, NULL);
    }

    if (!ctx.cfg.mute) {
        fprintf(stderr, "[System] 任务完成。耗时: %ld 秒\n", time(NULL) - ctx.state.start_time);
    }

    finalize_progress(&ctx.cfg, &ctx.state);
    app_context_destroy(&ctx);

    /* 释放命令行参数分配的字符串内存 */
    free(ctx.cfg.target_path);
    free(ctx.cfg.output_file);
    free(ctx.cfg.output_split_dir);
    free(ctx.cfg.progress_base);
    free(ctx.cfg.format);
    free(ctx.cfg.resume_file);

    return ctx.state.has_error ? 1 : 0;
}

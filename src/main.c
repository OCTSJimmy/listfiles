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

static void app_context_init(AppContext *ctx) {
    memset(ctx, 0, sizeof(AppContext));
    ctx->epfd = -1;
    ctx->running = false;
    ctx->fpbin_fd = -1;
    ctx->hist_pump_state = HIST_PUMP_DONE;
    ctx->next_requeue_worker = 0;
    atomic_init(&ctx->pending_tasks, 0);
}

static void app_context_destroy(AppContext *ctx) {
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
    if (ctx->fpbin_fd >= 0) {
        close(ctx->fpbin_fd);
        ctx->fpbin_fd = -1;
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

static void init_output_buffers(AppContext *ctx) {
    if (ctx->state.output_fp && ctx->state.output_fp != stdout) {
        setvbuf(ctx->state.output_fp, NULL, _IOFBF, 8 * 1024 * 1024);
    }
    if (ctx->state.dir_info_fp && ctx->state.dir_info_fp != stderr) {
        setvbuf(ctx->state.dir_info_fp, NULL, _IOFBF, 1 * 1024 * 1024);
    }
}

int main(int argc, char *argv[]) {
    AppContext ctx;
    app_context_init(&ctx);

    init_config(&ctx.cfg);
    if (parse_arguments(argc, argv, &ctx.cfg) != 0) {
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
    init_output_files(&ctx.cfg, &ctx.state);
    init_output_buffers(&ctx);
    ctx.async_writer = async_worker_init(&ctx.cfg, &ctx.state);

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
    int num_cores = get_nprocs();
    if (num_cores < 1) num_cores = 4;
    int num_workers = num_cores * 2;
    ctx.worker_pool = worker_pool_create(num_workers);
    ctx.probe_scheduler = probe_scheduler_create();

    if (!ctx.worker_pool || !ctx.probe_scheduler) {
        fprintf(stderr, "[Fatal] 无法初始化进程池\n");
        app_context_destroy(&ctx);
        return 1;
    }

    /* Spawn all workers before any restore/replay (needed for resume dispatch) */
    for (int i = 0; i < num_workers; i++) {
        worker_pool_spawn(ctx.worker_pool, i);
    }

    /* Resume mode: restore progress and replay unfinished tasks */
    if (ctx.cfg.continue_mode && !ctx.reference_set) {
        restore_progress(&ctx.cfg, &ctx);
    }

    /* Seed root task */
    struct stat root_info;
    if (lstat(ctx.cfg.target_path, &root_info) == 0) {
        if (S_ISDIR(root_info.st_mode)) {
            atomic_fetch_add(&ctx.pending_tasks, 1);
            uint32_t plen = (uint32_t)strlen(ctx.cfg.target_path);
            ipc_send(ctx.worker_pool->slots[0].fd_in, IPC_MSG_SCAN,
                     ctx.cfg.target_path, plen);
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

    fprintf(stderr, "[System] 任务开始...\n");
    main_loop_run(&ctx);

    fprintf(stderr, "[System] 任务完成。耗时: %ld 秒\n", time(NULL) - ctx.state.start_time);

    finalize_progress(&ctx.cfg, &ctx.state);
    app_context_destroy(&ctx);
    return ctx.state.has_error ? 1 : 0;
}

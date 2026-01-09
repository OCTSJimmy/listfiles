#include "config.h"
#include "cmdline.h"
#include "traversal.h"
#include "progress.h"
#include "utils.h"
#include "signals.h"
#include "monitor.h" 
#include "idempotency.h" // 确保包含 HashSet 定义
#include "device_manager.h" // [新增]
#include "output.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

// ==========================================
// 1. 会话管理逻辑
// ==========================================

static void load_session_config(Config *cfg) {
    if (!cfg->progress_base) return;
    char path[1024];
    snprintf(path, sizeof(path), "%s.config", cfg->progress_base);
    
    FILE *fp = fopen(path, "r");
    if (!fp) return; // 无历史文件，视为新任务

    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = line;
        char *val = eq + 1;
        val[strcspn(val, "\n")] = 0;

        if (strcmp(key, "path") == 0) {
            // 路径不匹配 -> 致命错误
            if (strcmp(cfg->target_path, val) != 0) {
                fprintf(stderr, "\n[错误] 检测到进度文件与当前路径不一致！\n");
                fprintf(stderr, "  历史记录: %s\n", val);
                fprintf(stderr, "  当前指定: %s\n", cfg->target_path);
                fprintf(stderr, "建议：使用 --runone 强制重跑，或检查 --progress-file 参数。\n");
                exit(1);
            }
        } else if (strcmp(key, "status") == 0) {
            if (strcmp(val, "Success") == 0) {
                // 上次成功 -> 标记为"可进行半增量"
                // 这里的逻辑是：continue_mode = true，后续通过 g_reference_history 是否加载成功来区分
                cfg->continue_mode = true; 
            } else if (strcmp(val, "Running") == 0) {
                // 上次未完成 -> 断点续传
                cfg->continue_mode = true;
            }
        } else if (strcmp(key, "archive") == 0) {
            // 归档策略必须一致
            bool hist_archive = atoi(val);
            if (hist_archive != cfg->archive) {
                fprintf(stderr, "[错误] 归档策略与历史记录不一致 (历史: %d, 当前: %d)\n", hist_archive, cfg->archive);
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
        printf("运行模式: 强制全量 (Fresh Start)\n");
    } else if (has_history && cfg->continue_mode) {
        // 这里只是初步判断，准确模式取决于后续 index 加载
        printf("运行模式: 智能续传/增量 (Smart Resume/Inc)\n");
    } else {
        printf("运行模式: 全量扫描 (Fresh Start)\n");
    }
    
    if (cfg->csv) printf("输出格式: CSV (Strict)\n");
    printf("半增量阈值: %ld 秒\n", cfg->skip_interval);

    printf("\n按 [Y] 继续，其他键退出: ");
    char c = getchar();
    if (c != 'y' && c != 'Y') {
        printf("已取消。\n");
        exit(0);
    }
}

int main(int argc, char *argv[]) {
    // 1. 初始化配置
    Config cfg;
    init_config(&cfg);
    if (parse_arguments(argc, argv, &cfg) != 0) {
        return 1;
    }
    // [修复 1] 预编译格式字符串！
    // 之前缺失了这一步，导致 format_segment_count 为 0，输出全是空行。
    precompile_format(&cfg);
    // 2. 信号处理
    setup_signal_handlers();

    // 3. 会话管理
    bool has_history = false;
    if (cfg.runone) {
        RuntimeState temp_state = {0}; 
        cleanup_progress(&cfg, &temp_state);
        cfg.continue_mode = false;
    } else {
        char path[1024];
        snprintf(path, sizeof(path), "%s.config", cfg.progress_base);
        if (access(path, F_OK) == 0) {
            has_history = true;
            load_session_config(&cfg);
        }
    }

    // 4. 交互确认
    interactive_confirm(&cfg, has_history);

    // 5. 初始化运行时状态
    RuntimeState state;
    memset(&state, 0, sizeof(RuntimeState));
    state.start_time = time(NULL);
    state.has_error = false; // [新增] 初始无错

    // [新增] 初始化设备管理器 (必须在 traverse_files 之前)
    state.dev_mgr = dev_mgr_create();

    // 保存配置
    if (!cfg.continue_mode || cfg.runone || !has_history) {
        save_config_to_disk(&cfg);
    }

    // 6. 预加载历史
    if (cfg.continue_mode && cfg.skip_interval > 0) {
        char path[1024];
        snprintf(path, sizeof(path), "%s.config", cfg.progress_base);
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
            printf("[System] 检测到上次任务已完成，正在加载历史索引以进行半增量扫描...\n");
            g_reference_history = hash_set_create(HASH_SET_INITIAL_SIZE);
            restore_progress_to_memory(&cfg, g_reference_history);
            printf("[System] 历史索引加载完成，元素数: %lu\n", (unsigned long)g_reference_history->element_count);
        }
    }

    // 7. 执行核心任务
    printf("[System] 任务开始...\n");
    traverse_files(&cfg, &state);

    // 8. 结束清理
    // [新增] 销毁设备管理器
    if (state.dev_mgr) {
        dev_mgr_destroy(state.dev_mgr);
        state.dev_mgr = NULL;
    }
    
    finalize_progress(&cfg, &state);
    // 如果不是 continue_mode (例如 runone)，traversal.c 没有清理缓存，这里可以补一下
    if (!cfg.continue_mode) {
        cleanup_progress(&cfg, &state);
    }
    printf("[System] 任务完成。耗时: %ld 秒\n", time(NULL) - state.start_time);
    
    // 如果有错误发生（熔断），返回非零状态码
    return state.has_error ? 1 : 0;
}
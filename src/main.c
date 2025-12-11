#include "config.h"
#include "cmdline.h"
#include "traversal.h"
#include "signals.h"
#include "output.h"
#include "idempotency.h"
#include "progress.h" // for save_config_to_disk
#include <string.h>
#include <stdlib.h>
#include <pthread.h>

int main(int argc, char *argv[]) {
    // 1. 初始化配置
    Config cfg;
    memset(&cfg, 0, sizeof(Config));
    init_config(&cfg);
    
    // 2. 解析参数
    if (parse_arguments(argc, argv, &cfg) != 0) {
        return 1; 
    }

    // 3. 系统环境初始化
    setup_signal_handlers();
    setvbuf(stdout, NULL, _IONBF, 0); 

    if (cfg.continue_mode) {
        g_history_object_set = hash_set_create(HASH_SET_INITIAL_SIZE);
    }

    // 4. 运行时状态与锁初始化
    RuntimeState state;
    memset(&state, 0, sizeof(RuntimeState));
    pthread_mutex_init(&state.dev_cache_mutex, NULL);
    
    if (acquire_lock(&cfg, &state) == -1) {
        pthread_mutex_destroy(&state.dev_cache_mutex);
        return 1;
    }

    // 5. 准备工作：保存配置、打开输出文件
    save_config_to_disk(&cfg);
    init_output_files(&cfg, &state);

    // 6. 执行核心遍历逻辑 (The Big Loop)
    traverse_files(&cfg, &state);

    // 7. 清理与退出
    if (cfg.format) {
        cleanup_compiled_format(&cfg);
    }
    if (state.output_fp && state.output_fp != stdout) {
        close_output_file(state.output_fp);
    }
    if (state.dir_info_fp && state.dir_info_fp != stderr) {
        close_output_file(state.dir_info_fp);
    }
    if (state.status_file_fp && state.status_file_fp != stdout) {
        fclose(state.status_file_fp);
    }

    release_lock(&state);

    if (g_history_object_set) {
        hash_set_destroy(g_history_object_set);
        g_history_object_set = NULL;
    }
    pthread_mutex_destroy(&state.dev_cache_mutex);
    
    // 清除屏幕状态行 (可选)
    printf("\033[11;0H");
    return 0;
}
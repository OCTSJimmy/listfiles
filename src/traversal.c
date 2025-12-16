#include "traversal.h"
#include "utils.h"
#include "smart_queue.h"
#include "async_worker.h"
#include "progress.h"
#include "output.h"
#include "idempotency.h"
#include "signals.h"
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/time.h>
#include <unistd.h>
#include <ctype.h> // for isspace

// 内部函数声明
static void load_resume_file(const Config *cfg, SmartQueue *queue);
static void process_directory(Config* cfg, RuntimeState* state, SmartQueue* queue, const char* dir_path);

// QoS 辅助函数
static long long current_timestamp() {
    struct timeval te; 
    gettimeofday(&te, NULL);
    return te.tv_sec*1000000LL + te.tv_usec;
}

// 主遍历函数
void traverse_files(Config *cfg, RuntimeState *state) {
    SmartQueue queue;
    init_smart_queue(&queue);

    verbose_printf(cfg, 1, "启动异步写入流水线...\n");
    async_worker_init(cfg, state);

    ThreadSharedState shared = {
        .cfg = cfg,
        .state = state,
        .queue = &queue,
        .running = 1
    };
    pthread_t status_thread;
    int thread_rc = pthread_create(&status_thread, NULL, status_thread_func, &shared);
    if (thread_rc != 0) {
        fprintf(stderr, "创建监控线程失败: %d\n", thread_rc);
        shared.running = 0;
    }

    if (cfg->continue_mode) {
        // 如果是导入模式 (-R)，清理旧进度并加载列表
        if (cfg->resume_file) {
            verbose_printf(cfg, 1, "检测到导入模式 (-R)，正在清理旧的进度文件...\n");
            cleanup_progress(cfg, state);
            
            // 重置状态
            state->process_slice_index = 0;
            state->processed_count = 0;
            state->write_slice_index = 0;
            
            progress_init(cfg, state);
            load_resume_file(cfg, &queue);
        } else {
            // 常规续传
            progress_init(cfg, state);
            restore_progress(cfg, &queue, state);
        }
    }
    
    // 如果不是 -R 模式，需要手动入队根目录
    if (!cfg->resume_file) {
        struct stat root_info;
        if (lstat(cfg->target_path, &root_info) == 0) {
            if (S_ISDIR(root_info.st_mode)) {
                smart_enqueue(cfg, &queue, cfg->target_path, &root_info);
            } else {
                // 【修复】：传入 &root_info
                push_write_task_file(cfg->target_path, &root_info);
                state->file_count++;
            }
        }
    }

    // === 主循环 ===
    while (true) {
        ScanNode *entry = smart_dequeue(cfg, &queue, state);
        if (!entry) break;
        state->total_dequeued_count++;
        // 运行时二次安检 (Lazy Validation)
        if (!entry->pre_checked) {
            struct stat info;
            
            // 1. 补做 lstat (IO)
            if (lstat(entry->path, &info) != 0) {
                verbose_printf(cfg, 2, "跳过失效的恢复路径: %s\n", entry->path);
                recycle_scan_node(&queue, entry);
                continue; 
            }

            // 2. 补做去重
            ObjectIdentifier id = { .st_dev = info.st_dev, .st_ino = info.st_ino };
            if (hash_set_contains(g_history_object_set, &id)) {
                recycle_scan_node(&queue, entry);
                continue;
            }
            hash_set_insert(g_history_object_set, &id);

            // 3. 补做转录
            if (cfg->continue_mode) {
                record_path(cfg, state, entry->path, &info);
            }
            
            // 4. 处理文件 (传递缓存的 stat 信息!)
            if (S_ISDIR(info.st_mode)) {
                // 如果是目录，进入 process_directory 扫描子项
                if (cfg->include_dir || cfg->print_dir) {
                    // 【修复】：传入 &info
                    if(cfg->include_dir) push_write_task_file(entry->path, &info);
                }
                state->current_path = entry->path;
                process_directory(cfg, state, &queue, entry->path);
            } else {
                // 如果是文件（可能 load_resume_file 里包含文件），直接输出
                push_write_task_file(entry->path, &info);
            }
        } else {
             // smart_enqueue 进来的已经是通过 check 的目录
             state->current_path = entry->path;
             process_directory(cfg, state, &queue, entry->path);
        }
        
        // 提交进度快照
        if (cfg->continue_mode) {
            push_write_task_checkpoint(state);
        }

        recycle_scan_node(&queue, entry);
    }
    
    verbose_printf(cfg, 1, "遍历完成，等待数据落盘...\n");

    async_worker_shutdown();
    shared.running = 0;
    if (thread_rc == 0) pthread_join(status_thread, NULL);

    cleanup_smart_queue(&queue);
    cleanup_cache(state);
    if (cfg->continue_mode) {
        cleanup_progress(cfg, state);
        release_lock(state);
    }
}

static void process_directory(Config* cfg, RuntimeState* state, SmartQueue* queue, const char* dir_path) {
    DIR *dir = opendir(dir_path);
    if (!dir) {
        verbose_printf(cfg, 1, "无法打开目录: %s\n", dir_path);
        return;
    }

    long long current_sleep = START_SLEEP_US; 
    long long op_start, op_end, duration;
    
    struct dirent *child_entry;
    while (true) {
        op_start = current_timestamp();
        child_entry = readdir(dir);
        op_end = current_timestamp();

        if (child_entry == NULL) break;

        // QoS 动态调节
        duration = op_end - op_start;
        if (duration < 2000) { 
            current_sleep = current_sleep * 0.95;
            if (current_sleep < MIN_SLEEP_US) current_sleep = MIN_SLEEP_US;
        } else if (duration > 100000) { 
            current_sleep = (current_sleep * 2) + 50000;
        } else if (duration > 10000) {
            current_sleep = current_sleep * 1.1;
        }
        if (current_sleep > MAX_SLEEP_US) current_sleep = MAX_SLEEP_US;
        if (current_sleep > 0) usleep(current_sleep);

        if (child_entry->d_name[0] == '.' && 
           (child_entry->d_name[1] == '\0' || (child_entry->d_name[1] == '.' && child_entry->d_name[2] == '\0'))) {
            continue;
        }

        char full_path[MAX_PATH_LENGTH];
        int n = snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, child_entry->d_name);
        if (n >= MAX_PATH_LENGTH) continue;

        bool is_dir = false;
        bool need_stat = false;
        struct stat info;
        memset(&info, 0, sizeof(info));

        // 决定是否需要 stat
        if (cfg->size || cfg->mtime || cfg->atime || cfg->user || cfg->group || cfg->continue_mode || cfg->format) {
            need_stat = true;
        }

        // d_type 优化
        if (child_entry->d_type != DT_UNKNOWN) {
            if (child_entry->d_type == DT_DIR) is_dir = true;
            if (need_stat) {
                if (lstat(full_path, &info) != 0) {
                    verbose_printf(cfg, 1, "无法获取文件状态: %s\n", full_path);
                    continue;
                }
            }
        } else {
            if (lstat(full_path, &info) != 0) continue;
            if (S_ISDIR(info.st_mode)) is_dir = true;
        }

        if (is_dir) {
            // 目录：入队，查重，记录进度
            smart_enqueue(cfg, queue, full_path, &info);
            if (cfg->continue_mode) {
                record_path(cfg, state, full_path, &info);
            }
            state->dir_count++;
            if (cfg->print_dir) {
                fprintf(state->dir_info_fp, OUTPUT_DIR_PREFIX "%s\n", full_path);
            }
            if (cfg->include_dir) {
                push_write_task_file(full_path, &info);
            }
        } else {
            // 文件：直接推给 Worker (传递 stat info!)
            push_write_task_file(full_path, &info);
        }
    }
    closedir(dir);
}

static void load_resume_file(const Config *cfg, SmartQueue *queue) {
    FILE *fp = fopen(cfg->resume_file, "r");
    if (!fp) {
        perror("无法打开恢复列表文件");
        exit(EXIT_FAILURE);
    }

    verbose_printf(cfg, 1, "正在从 %s 加载任务列表...\n", cfg->resume_file);

    char line[MAX_PATH_LENGTH + 256];
    unsigned long loaded_count = 0;
    char path_buf[MAX_PATH_LENGTH]; 
    size_t prefix_len = strlen(OUTPUT_DIR_PREFIX);

    while (fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\n")] = 0;
        char *path_start = line;
        while (isspace((unsigned char)*path_start)) path_start++;

        if (strncmp(path_start, OUTPUT_DIR_PREFIX, prefix_len) == 0) {
            path_start += prefix_len;
        }

        size_t len = strlen(path_start);
        if (len > 1 && path_start[0] == '"' && path_start[len-1] == '"') {
            size_t inner_len = len - 2;
            if (inner_len >= sizeof(path_buf)) inner_len = sizeof(path_buf) - 1;
            strncpy(path_buf, path_start + 1, inner_len);
            path_buf[inner_len] = '\0';
            path_start = path_buf;
        }

       if (*path_start == '\0') continue;
        
        // 盲入队：速度极快，不执行 IO
        blind_enqueue(queue, path_start);
        loaded_count++;
    }

    fclose(fp);
    verbose_printf(cfg, 1, "列表加载完成，队列预热 %lu 项。\n", loaded_count);
}
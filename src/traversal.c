#include "traversal.h"
#include "looper.h"
#include "utils.h"
#include "async_worker.h"
#include "progress.h"
#include "idempotency.h"
#include "signals.h"
#include "output.h" // 确保包含 format_elapsed_time 等
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/sysinfo.h>

// === 全局队列与状态 ===
// 主线程收信箱 (处理结果) - 仅由主线程访问，无需额外锁保护
static MessageQueue g_looper_mq;
// 工人抢任务池 (处理IO) - 由主线程和多个Worker线程并发访问，内部已实现线程安全
static MessageQueue g_worker_mq;
// 正在进行的任务数 (用于判断何时结束) - 使用atomic类型确保原子操作，线程安全
static atomic_long g_pending_tasks = 0;

// 特殊消息：任务完成通知
#define MSG_TASK_DONE 999 

// ==========================================
// 1. Worker 线程逻辑 (消费者：只负责IO和打包)
// ==========================================

// [IO密集] 扫描单个目录并批量上报
// 注意：该函数在Worker线程中执行，需要保证线程安全
// - 不直接修改全局状态，仅通过消息队列与主线程通信
// - 使用本地变量避免线程竞争
static void worker_scan_dir(const char *dir_path) {
    DIR *dir = opendir(dir_path);
    if (!dir) {
        // 无法打开目录，记录错误 (不中断流程)
        // 使用 stderr 记录错误，因为 worker 线程没有 cfg 指针
        fprintf(stderr, "无法打开目录: %s, 错误: %s\n", dir_path, strerror(errno));
        return;
    }

    // 创建一个任务批次 (篮子)
    TaskBatch *batch = batch_create();
    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL) {
        // 跳过 . 和 ..
        if (entry->d_name[0] == '.' && 
           (entry->d_name[1] == '\0' || (entry->d_name[1] == '.' && entry->d_name[2] == '\0'))) {
            continue;
        }

        char full_path[MAX_PATH_LENGTH];
        int n = snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);
        if (n >= MAX_PATH_LENGTH) {
            // 路径过长被截断，记录错误
            fprintf(stderr, "警告: 路径过长被截断: %s/%s\n", dir_path, entry->d_name);
            continue;
        }

        struct stat info;
        // 执行 lstat (这是最耗时的 IO 操作之一)
        if (lstat(full_path, &info) == 0) {
            // 加入批次
            batch_add(batch, full_path, &info);

            // 篮子满了？发送！
            if (batch->count >= BATCH_SIZE) {
                mq_send(&g_looper_mq, MSG_RESULT_BATCH, batch);
                batch = batch_create(); // 换新篮子
            }
        } else {
            // 无法获取文件状态，记录错误
            fprintf(stderr, "无法获取文件状态: %s, 错误: %s\n", full_path, strerror(errno));
        }
    }
    
    // 发送剩余的半篮子
    if (batch->count > 0) {
        mq_send(&g_looper_mq, MSG_RESULT_BATCH, batch);
    } else {
        batch_destroy(batch); // 空篮子直接销毁
    }

    closedir(dir);
}

// [IO密集] 批量验证路径 (用于 Resume)
static void worker_check_batch(TaskBatch *input_batch) {
    TaskBatch *result_batch = batch_create();
    
    // 遍历输入的一批路径，验证是否存在
    for (int i = 0; i < input_batch->count; i++) {
        struct stat info;
        if (lstat(input_batch->paths[i], &info) == 0) {
            // 存在的有效路径，加入结果批次
            batch_add(result_batch, input_batch->paths[i], &info);
        }
    }
    
    // 将有效结果发回 Looper
    if (result_batch->count > 0) {
        mq_send(&g_looper_mq, MSG_RESULT_BATCH, result_batch);
    } else {
        batch_destroy(result_batch);
    }
    
    // 销毁输入的任务包
    batch_destroy(input_batch);
}

// Worker 线程入口函数
// 注意：该函数是Worker线程的入口点，需要保证线程安全
// - 通过消息队列与主线程通信，内部已实现线程安全
// - 每个Worker线程独立执行，互不干扰
// - 动态分配的内存（如strdup的路径）需要在使用后及时释放
static void* worker_thread_entry(void *arg) {
    (void)arg;
    while (true) {
        // 阻塞等待任务
        Message *msg = mq_dequeue(&g_worker_mq);
        if (!msg) break; // 队列被销毁，退出线程

        switch (msg->what) {
            case MSG_SCAN_DIR:
                // 处理目录扫描任务
                worker_scan_dir((char *)msg->obj);
                free(msg->obj); // 释放路径字符串
                // 通知 Looper 这里的活干完了
                mq_send(&g_looper_mq, MSG_TASK_DONE, NULL);
                break;
                
            case MSG_CHECK_BATCH:
                // 处理批量检查任务
                worker_check_batch((TaskBatch *)msg->obj);
                // 通知 Looper 这里的活干完了
                mq_send(&g_looper_mq, MSG_TASK_DONE, NULL);
                break;
        }
        
        // 回收消息对象
        mq_recycle(&g_worker_mq, msg);
    }
    return NULL;
}

// ==========================================
// 2. Looper 主控逻辑 (生产者/调度者)
// ==========================================

// 辅助：加载 Resume 文件并派发任务
static void dispatch_resume_file(const Config *cfg) {
    FILE *fp = fopen(cfg->resume_file, "r");
    if (!fp) {
        perror("无法打开恢复列表文件");
        return;
    }

    char line[MAX_PATH_LENGTH + 256];
    char path_buf[MAX_PATH_LENGTH];
    size_t prefix_len = strlen(OUTPUT_DIR_PREFIX);
    
    TaskBatch *batch = batch_create();

    while (fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\n")] = 0;
        char *path_start = line;
        while (*path_start == ' ' || *path_start == '\t') path_start++;

        // 去除 "目录: " 前缀
        if (strncmp(path_start, OUTPUT_DIR_PREFIX, prefix_len) == 0) {
            path_start += prefix_len;
        }
        
        // 去除双引号 (简单的处理)
        size_t len = strlen(path_start);
        if (len > 1 && path_start[0] == '"' && path_start[len-1] == '"') {
            size_t inner_len = len - 2;
            if (inner_len >= sizeof(path_buf)) {
                // 路径过长被截断，记录错误
                fprintf(stderr, "警告: 恢复文件中的路径过长被截断: %s\n", path_start);
                continue;
            }
            strncpy(path_buf, path_start + 1, inner_len);
            path_buf[inner_len] = '\0';
            path_start = path_buf;
        }
        
        if (*path_start == '\0') continue;

        // 加入待检查批次 (stat 暂时空着，由 Worker 填充)
        batch_add(batch, path_start, NULL);

        // 攒够一批，发给 Worker
        if (batch->count >= BATCH_SIZE) {
            atomic_fetch_add(&g_pending_tasks, 1); // 记账：发出了一个任务
            mq_send(&g_worker_mq, MSG_CHECK_BATCH, batch);
            batch = batch_create();
        }
    }
    
    // 发送最后剩余的
    if (batch->count > 0) {
        atomic_fetch_add(&g_pending_tasks, 1);
        mq_send(&g_worker_mq, MSG_CHECK_BATCH, batch);
    } else {
        batch_destroy(batch);
    }
    fclose(fp);
}

// 核心循环：Looper
// 注意：该函数是主线程的核心逻辑，负责协调所有Worker线程
// - 初始化和销毁消息队列
// - 启动和停止Worker线程池
// - 派发初始任务
// - 处理Worker线程返回的结果
// - 维护全局任务计数（使用atomic类型确保线程安全）
// - 线程安全注意：所有对全局状态的修改都需要保证线程安全
static void run_main_looper(Config *cfg, RuntimeState *state) {
    // 1. 初始化双队列
    mq_init(&g_looper_mq);
    mq_init(&g_worker_mq);
    atomic_store(&g_pending_tasks, 0);

    // 2. 启动 Worker 线程池
    // 根据 CPU 核心数动态调整线程池大小，IO 密集型任务可以使用核心数的 2-4 倍
    int num_cores = sysconf(_SC_NPROCESSORS_ONLN);
    if (num_cores < 1) num_cores = 4; // 保底
    int num_workers = num_cores * 2; 
    pthread_t *workers = safe_malloc(sizeof(pthread_t) * num_workers);
    for (int i = 0; i < num_workers; i++) {
        pthread_create(&workers[i], NULL, worker_thread_entry, NULL);
    }
    verbose_printf(cfg, 1, "启动 %d 个 Worker 线程\n", num_workers);

    // 3. 派发初始任务 (种子)
    if (cfg->continue_mode) {
        bool resumed = false;

        if (cfg->resume_file) {
            // -R 模式：从文本列表恢复
            verbose_printf(cfg, 1, "正在并行加载恢复列表 (-R)...\n");
            dispatch_resume_file(cfg); 
            resumed = true;
        } else {
            // --continue 模式：尝试从 checkpoint 恢复
            // 【新增：调用加载索引】
            if (load_progress_index(cfg, state)) {
                verbose_printf(cfg, 1, "发现断点，正在恢复进度 (Slice: %lu, Items: %lu)...\n", 
                               state->process_slice_index, state->processed_count);
                
                // 恢复分片数据
                int batches = restore_progress(cfg, &g_worker_mq, state);
                atomic_fetch_add(&g_pending_tasks, batches);
            } else {
                verbose_printf(cfg, 1, "未找到有效断点，将从头开始扫描。\n");
            }
        }
        // 如果没有恢复任何进度（或者恢复失败），则从根目录开始
        if (!resumed) {
            struct stat root_info;
            if (lstat(cfg->target_path, &root_info) == 0) {
                 if (S_ISDIR(root_info.st_mode)) {
                    atomic_fetch_add(&g_pending_tasks, 1);
                    mq_send(&g_worker_mq, MSG_SCAN_DIR, strdup(cfg->target_path));
                } else {
                    push_write_task_file(cfg->target_path, &root_info);
                    state->file_count++;
                }
            }
        }
    } else {
        // 扫描根目录
        struct stat root_info;
        if (lstat(cfg->target_path, &root_info) == 0) {
            if (S_ISDIR(root_info.st_mode)) {
                // 如果是目录，发给 Worker 去扫
                atomic_fetch_add(&g_pending_tasks, 1);
                mq_send(&g_worker_mq, MSG_SCAN_DIR, strdup(cfg->target_path));
            } else {
                // 如果是单文件，直接输出
                push_write_task_file(cfg->target_path, &root_info);
                state->file_count++;
            }
        } else {
            // 无法获取根目录状态，记录错误
            fprintf(stderr, "无法获取根目录状态: %s, 错误: %s\n", cfg->target_path, strerror(errno));
        }
    }

    // 4. Looper 事件循环 (主线程在此空转)
    while (true) {
        // 退出条件：所有派发出去的任务都已回报(done)，且 Looper 队列里没积压消息
        if (atomic_load(&g_pending_tasks) == 0 && g_looper_mq.head == NULL) {
            break;
        }

        // 阻塞等待消息
        Message *msg = mq_dequeue(&g_looper_mq);
        if (!msg) break; // 应该不会发生

        switch (msg->what) {
            case MSG_RESULT_BATCH: {
                TaskBatch *batch = (TaskBatch *)msg->obj;
                
                // === 串行处理一批结果 (无锁去重) ===
                for (int i = 0; i < batch->count; i++) {
                    char *path = batch->paths[i];
                    struct stat *st = &batch->stats[i];

                    // A. 幂等性检查 (这里不需要锁，因为 Looper 是单线程的)
                    ObjectIdentifier id = {st->st_dev, st->st_ino};
                    if (hash_set_contains(g_history_object_set, &id)) {
                        continue; // 重复，跳过
                    }
                    hash_set_insert(g_history_object_set, &id);

                    // B. 逻辑分流
                    if (S_ISDIR(st->st_mode)) {
                        // 如果是目录，派发新任务给 Worker
                        atomic_fetch_add(&g_pending_tasks, 1);
                        mq_send(&g_worker_mq, MSG_SCAN_DIR, strdup(path));
                        
                        state->dir_count++;
                        
                        // 目录本身输出
                        if (cfg->include_dir || cfg->print_dir) {
                             if(cfg->include_dir) push_write_task_file(path, st);
                             if(cfg->print_dir) {
                                 // 直接写 stderr 或者通过 async worker
                                 // 这里为了简单保持原样，注意线程安全
                                 // fprintf(stderr, "目录: %s\n", path);
                             }
                        }
                        
                        // 记录断点 (可选)
                        if (cfg->continue_mode) {
                            record_path(cfg, state, path, st);
                        }
                    } else {
                        // 如果是文件，直接输出
                        state->file_count++;
                        push_write_task_file(path, st);
                    }
                }
                // 处理完一批，销毁这个批次数据
                batch_destroy(batch);
                break;
            }

            case MSG_TASK_DONE:
                // 收到 Worker 完成通知
                atomic_fetch_sub(&g_pending_tasks, 1);
                state->total_dequeued_count++; // 用于速率统计
                break;
        }

        // 回收消息壳
        mq_recycle(&g_looper_mq, msg);
    }

    verbose_printf(cfg, 1, "所有扫描任务已完成。\n");

    // 5. 清理现场
    // 销毁 Worker 队列 -> Worker 线程会读到 NULL 并退出
    mq_destroy(&g_worker_mq);
    
    for (int i = 0; i < num_workers; i++) {
        pthread_join(workers[i], NULL);
    }
    free(workers);
    
    mq_destroy(&g_looper_mq);
}

// ==========================================
// 3. 对外接口 (traverse_files)
// ==========================================

void traverse_files(Config *cfg, RuntimeState *state) {
    // 1. 初始化哈希集合用于幂等性检查
    if (!g_history_object_set) {
        g_history_object_set = hash_set_create(HASH_SET_INITIAL_SIZE);
    }
    
    // 2. 启动 AsyncWorker (写文件线程)
    // 这是独立的，用于写磁盘，不受 Looper 架构影响
    async_worker_init(cfg, state);
    
    // 2. 启动监控线程 (可选)
    // 注意：这里我们不再传递 Queue，因为 SmartQueue 已废弃
    // 需要修改 display_status 适配新的统计方式
    ThreadSharedState shared = {
        .cfg = cfg,
        .state = state,
        .running = 1
    };
    pthread_t status_thread;
    pthread_create(&status_thread, NULL, status_thread_func, &shared);

    // 3. 运行主循环 (阻塞直到完成)
    run_main_looper(cfg, state);

    // 4. 关闭流程
    verbose_printf(cfg, 1, "等待异步写入完成...\n");
    async_worker_shutdown();
    
    shared.running = 0;
    pthread_join(status_thread, NULL);
    
    // 5. 清理资源
    if (cfg->continue_mode) {
        cleanup_progress(cfg, state);
        release_lock(state);
    }
    // SmartQueue 的清理已不需要调用，因为我们没用到它
    cleanup_cache(state);
    
    // 6. 销毁哈希集合
    if (g_history_object_set) {
        hash_set_destroy(g_history_object_set);
        g_history_object_set = NULL;
    }
}
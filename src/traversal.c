#include "traversal.h"
#include "looper.h"
#include "utils.h"
#include "async_worker.h"
#include "progress.h"
#include "idempotency.h"
#include "signals.h"
#include "output.h"
#include "monitor.h"
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
static MessageQueue g_looper_mq;
static MessageQueue g_worker_mq;
static atomic_long g_pending_tasks = 0;

// === 外部全局变量 (Idempotency) ===
// 1. Looper 专用：本次扫描的访问记录 (用于防止环路)
extern HashSet* g_visited_history; 
// 2. Worker 专用：上次扫描的历史索引 (只读，用于半增量跳过)
extern HashSet* g_reference_history;

// === 本地结构 ===
// 恢复线程参数
typedef struct {
    const Config *cfg;
    RuntimeState *state;
    MessageQueue *target_mq;
} ResumeThreadArgs;

// Worker 线程参数
typedef struct {
    const Config *cfg;
} WorkerArgs;

// 低优先级队列节点 (用于 Looper 暂存新任务)
typedef struct LowPriNode {
    char *path;
    struct LowPriNode *next;
} LowPriNode;

// ==========================================
// 1. Worker 线程逻辑
// ==========================================

// 辅助：将 mode_t 转换为 d_type
static unsigned char get_dtype_from_stat(mode_t mode) {
    if (S_ISREG(mode)) return DT_REG;
    if (S_ISDIR(mode)) return DT_DIR;
    if (S_ISLNK(mode)) return DT_LNK;
    if (S_ISCHR(mode)) return DT_CHR;
    if (S_ISBLK(mode)) return DT_BLK;
    if (S_ISFIFO(mode)) return DT_FIFO;
    if (S_ISSOCK(mode)) return DT_SOCK;
    return DT_UNKNOWN;
}

// 核心扫描函数：包含半增量跳过逻辑
static void worker_scan_dir(const Config *cfg, const char *dir_path) {
    DIR *dir = opendir(dir_path);
    if (!dir) {
        // 权限不足或目录消失是常态，记录错误但不崩溃
        // fprintf(stderr, "无法打开目录: %s, 错误: %s\n", dir_path, strerror(errno));
        return;
    }

    // 获取当前目录设备ID (用于构建 key)
    // 假设: 同一目录下的文件通常在同一设备上 (跨设备挂载点除外，这是为了性能的妥协)
    struct stat dir_stat;
    dev_t current_dev = 0;
    if (lstat(dir_path, &dir_stat) == 0) {
        current_dev = dir_stat.st_dev;
    }

    TaskBatch *batch = batch_create();
    struct dirent *entry;
    time_t now = time(NULL);

    while ((entry = readdir(dir)) != NULL) {
        // 跳过 "." 和 ".."
        if (entry->d_name[0] == '.' && 
           (entry->d_name[1] == '\0' || (entry->d_name[1] == '.' && entry->d_name[2] == '\0'))) {
            continue;
        }

        char full_path[MAX_PATH_LENGTH];
        int n = snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);
        if (n >= MAX_PATH_LENGTH) continue; // 路径过长忽略

        // === [Phase 3 核心: 半增量跳过逻辑] ===
        bool skip_lstat = false;
        struct stat cached_info = {0};

        // 启用条件: 配置了间隔 > 0 && 存在参考历史 && inode 有效
        if (cfg->skip_interval > 0 && g_reference_history && entry->d_ino != 0) {
            // 1. 查表 (O(1))
            HashSetNode *node = hash_set_lookup(g_reference_history, current_dev, entry->d_ino);
            
            if (node) {
                // 2. 校验链 (Verify Chain)
                
                // A. 文件名哈希校验 (防 Inode 复用)
                uint32_t name_hash = calculate_name_hash(entry->d_name);
                bool hash_match = (node->id.name_hash == name_hash);
                
                // B. 类型校验 (d_type 可能为 DT_UNKNOWN，此时不强校验)
                bool type_match = true;
                if (entry->d_type != DT_UNKNOWN) {
                    type_match = (node->id.d_type == entry->d_type);
                }

                // C. 时间区间校验
                if (hash_match && type_match) {
                    double age = difftime(now, node->id.mtime);
                    if (age > cfg->skip_interval) {
                        // === HIT! 判定为"死数据"，跳过 lstat ===
                        skip_lstat = true;
                        
                        // 构造伪 stat 信息 (仅填充关键字段)
                        cached_info.st_ino = entry->d_ino;
                        cached_info.st_dev = current_dev;
                        cached_info.st_mtime = node->id.mtime;
                        cached_info.st_atime = node->id.mtime; // 近似值
                        cached_info.st_ctime = node->id.mtime; // 近似值
                        
                        // 尽可能恢复 st_mode 类型位
                        if (node->id.d_type == DT_DIR) cached_info.st_mode = S_IFDIR | 0755;
                        else if (node->id.d_type == DT_REG) cached_info.st_mode = S_IFREG | 0644;
                        else if (node->id.d_type == DT_LNK) cached_info.st_mode = S_IFLNK | 0777;
                        else cached_info.st_mode = 0;
                        
                        // 注意: size 无法从 Inode 恢复，半增量模式下 size 将为 0
                        cached_info.st_size = 0; 
                    }
                }
            }
        }
        // ========================================

        if (skip_lstat) {
            // 极速路径：使用缓存元数据
            batch_add(batch, full_path, &cached_info);
        } else {
            // 传统路径：执行 IO
            struct stat info;
            if (lstat(full_path, &info) == 0) {
                batch_add(batch, full_path, &info);
            }
        }

        // 批次满了发送
        if (batch->count >= BATCH_SIZE) {
            mq_send(&g_looper_mq, MSG_RESULT_BATCH, batch);
            batch = batch_create();
        }
    }
    
    // 发送剩余批次
    if (batch->count > 0) mq_send(&g_looper_mq, MSG_RESULT_BATCH, batch);
    else batch_destroy(batch);

    closedir(dir);
}

// 批量检查 (用于 Resume 阶段验证文件是否存在)
static void worker_check_batch(TaskBatch *input_batch) {
    TaskBatch *result_batch = batch_create();
    for (int i = 0; i < input_batch->count; i++) {
        struct stat info;
        // Resume 检查必须 lstat，确保文件还在
        if (lstat(input_batch->paths[i], &info) == 0) {
            batch_add(result_batch, input_batch->paths[i], &info);
        }
    }
    if (result_batch->count > 0) mq_send(&g_looper_mq, MSG_RESULT_BATCH, result_batch);
    else batch_destroy(result_batch);
    batch_destroy(input_batch);
}

// Worker 线程入口
static void* worker_thread_entry(void *arg) {
    WorkerArgs *args = (WorkerArgs*)arg;
    const Config *cfg = args->cfg;
    free(args); // 释放参数容器

    while (true) {
        Message *msg = mq_dequeue(&g_worker_mq);
        if (!msg) break; // 收到 NULL (Quit)

        switch (msg->what) {
            case MSG_SCAN_DIR:
                worker_scan_dir(cfg, (char *)msg->obj);
                free(msg->obj); // 路径字符串使用完释放
                mq_send(&g_looper_mq, MSG_TASK_DONE, NULL);
                break;
                
            case MSG_CHECK_BATCH:
                worker_check_batch((TaskBatch *)msg->obj);
                mq_send(&g_looper_mq, MSG_TASK_DONE, NULL);
                break;
        }
        mq_recycle(&g_worker_mq, msg);
    }
    return NULL;
}

// ==========================================
// 2. Resume 线程 (流控生产者)
// ==========================================

static void *resume_thread_entry(void *arg) {
    ResumeThreadArgs *args = (ResumeThreadArgs *)arg;
    
    // 执行恢复逻辑 (progress.c)
    // 这里的 restore_progress 会读取 .pbin/.archive 并发送 batch 到 target_mq
    restore_progress(args->cfg, args->target_mq, args->state);
    
    // 通知 Looper 恢复已完成
    mq_send(&g_looper_mq, MSG_RESUME_FINISHED, NULL);
    
    free(args);
    return NULL;
}

// ==========================================
// 3. Looper 主控逻辑 (调度者)
// ==========================================

static void run_main_looper(const Config *cfg, RuntimeState *state, AsyncWorker *worker) {
    mq_init(&g_looper_mq);
    mq_init(&g_worker_mq);
    atomic_store(&g_pending_tasks, 0);

    // 1. 初始化本次访问集合 (Looper 独占，防环用)
    g_visited_history = hash_set_create(HASH_SET_INITIAL_SIZE);

    // 2. 启动 Worker 线程池
    int num_cores = get_nprocs();
    if (num_cores < 1) num_cores = 4;
    int num_workers = num_cores * 2; // IO 密集型可适当超配
    
    pthread_t *workers = safe_malloc(sizeof(pthread_t) * num_workers);
    for (int i = 0; i < num_workers; i++) {
        WorkerArgs *args = safe_malloc(sizeof(WorkerArgs));
        args->cfg = cfg;
        pthread_create(&workers[i], NULL, worker_thread_entry, args);
    }
    verbose_printf(cfg, 1, "启动 %d 个 Worker 线程\n", num_workers);

    // 3. 初始任务分发 & 模式选择
    bool resume_active = false;
    
    // 低优先级队列 (链表，用于 Resume 期间积压新任务)
    LowPriNode *low_pri_head = NULL;
    LowPriNode *low_pri_tail = NULL;
    int low_pri_count = 0;

    // A. 强制全量 (--runone)
    if (cfg->runone) {
        verbose_printf(cfg, 1, "模式: 强制全量扫描 (--runone)\n");
        // 下发根目录逻辑在后方统一处理
    }
    // B. 智能续传 / 半增量
    else if (cfg->continue_mode) {
        // 如果 g_reference_history 已存在(由 main.c 预加载)，说明是“半增量模式”
        if (g_reference_history) {
            verbose_printf(cfg, 1, "模式: 半增量扫描 (Reference Loaded)\n");
            // 半增量本质上也是从根目录重新扫，只是利用 reference 跳过 lstat
            // 所以这里不需要启动 resume 线程，直接下发根目录即可
        } else {
            // 如果 reference 未加载且 continue_mode=true，说明是“断点续传模式”
            // 需要加载 .idx 并恢复队列
            if (load_progress_index(cfg, state)) {
                verbose_printf(cfg, 1, "模式: 断点续传 (Resuming)...\n");
                
                // 启动恢复线程
                ResumeThreadArgs *r_args = safe_malloc(sizeof(ResumeThreadArgs));
                r_args->cfg = cfg;
                r_args->state = state;
                r_args->target_mq = &g_worker_mq;
                
                pthread_t r_tid;
                pthread_create(&r_tid, NULL, resume_thread_entry, r_args);
                pthread_detach(r_tid); // 独立运行
                
                resume_active = true;
            }
        }
    }

    // C. 如果没有正在进行 Resume，则需要下发根目录作为种子
    if (!resume_active) {
        struct stat root_info;
        if (lstat(cfg->target_path, &root_info) == 0) {
            if (S_ISDIR(root_info.st_mode)) {
                atomic_fetch_add(&g_pending_tasks, 1);
                mq_send(&g_worker_mq, MSG_SCAN_DIR, strdup(cfg->target_path));
            } else {
                // 如果目标本身只是个文件
                push_write_task_file(worker, cfg->target_path, &root_info);
                state->file_count++;
            }
        } else {
            fprintf(stderr, "错误: 无法访问目标路径 %s\n", cfg->target_path);
        }
    }

    // 4. 主事件循环
    while (true) {
        // 退出条件检查:
        // 1. 没有 Pending 任务 (所有发出的任务都 done 了)
        // 2. Looper 队列空了
        // 3. Resume 线程结束了
        // 4. 低优先级队列空了
        if (atomic_load(&g_pending_tasks) == 0 && 
            g_looper_mq.head == NULL && 
            !resume_active && 
            low_pri_head == NULL) {
            break;
        }

        // 阻塞等待 Looper 消息
        Message *msg = mq_dequeue(&g_looper_mq);
        if (!msg) break;

        switch (msg->what) {
            case MSG_RESULT_BATCH: {
                TaskBatch *batch = (TaskBatch *)msg->obj;
                TaskBatch *output_batch = batch_create();
                
                for (int i = 0; i < batch->count; i++) {
                    char *path = batch->paths[i];
                    struct stat *st = &batch->stats[i];

                    // 1. 防环检查 (Looper 独占访问 g_visited_history，无锁)
                    ObjectIdentifier id = { .st_dev = st->st_dev, .st_ino = st->st_ino };
                    if (hash_set_contains(g_visited_history, &id)) {
                        continue; // 环路或重复，跳过
                    }
                    hash_set_insert(g_visited_history, &id);

                    // 2. 处理逻辑
                    if (S_ISDIR(st->st_mode)) {
                        // === 发现目录：产生新任务 ===
                        
                        // [分级流控逻辑]
                        if (resume_active) {
                            // 恢复进行中 -> 压入低优先级队列，暂不发给 Worker
                            LowPriNode *node = safe_malloc(sizeof(LowPriNode));
                            node->path = strdup(path);
                            node->next = NULL;
                            
                            if (low_pri_tail) low_pri_tail->next = node;
                            else low_pri_head = node;
                            low_pri_tail = node;
                            low_pri_count++;
                        } else {
                            // 正常发送给 Worker
                            atomic_fetch_add(&g_pending_tasks, 1);
                            mq_send(&g_worker_mq, MSG_SCAN_DIR, strdup(path));
                        }

                        state->dir_count++;
                        
                        // 目录输出与记录
                        if (cfg->include_dir) batch_add(output_batch, path, st);
                        if (cfg->print_dir && state->dir_info_fp) {
                            fprintf(state->dir_info_fp, "%s%s\n", OUTPUT_DIR_PREFIX, path);
                        }
                        // 只有在 continue 模式下才记录进度到 pbin
                        if (cfg->continue_mode) record_path(cfg, state, path, st);
                        
                    } else {
                        // === 发现文件：直接输出 ===
                        state->file_count++;
                        batch_add(output_batch, path, st);
                    }
                }
                
                // 批量推送给 Writer 线程
                if (output_batch->count > 0) push_write_task_batch(worker, output_batch);
                else batch_destroy(output_batch);
                
                batch_destroy(batch); // 释放原始 batch
                break;
            }

            case MSG_TASK_DONE:
                atomic_fetch_sub(&g_pending_tasks, 1);
                state->total_dequeued_count++;
                break;

            case MSG_RESUME_FINISHED:
                verbose_printf(cfg, 1, "恢复阶段完成。释放低优先级任务 (%d 个)...\n", low_pri_count);
                resume_active = false;
                
                // 泄洪：将积压在低优先级队列的任务全部灌入 Worker
                LowPriNode *curr = low_pri_head;
                while (curr) {
                    atomic_fetch_add(&g_pending_tasks, 1);
                    // 注意：直接移交 curr->path 指针，不 strdup，也不 free(path)
                    // mq_send 内部是 strdup 吗？ looper.c 中 mq_send -> mq_obtain(..., obj) 是直接赋值的
                    // Worker 中使用完会 free(msg->obj)
                    // 所以这里直接传 curr->path 是安全的
                    mq_send(&g_worker_mq, MSG_SCAN_DIR, curr->path); 
                    
                    LowPriNode *next = curr->next;
                    free(curr); // 只释放节点容器
                    curr = next;
                }
                low_pri_head = low_pri_tail = NULL;
                low_pri_count = 0;
                break;
                
            case MSG_WORKER_STUCK:
                // 简单的日志记录，可扩展为报警
                verbose_printf(cfg, 0, "警告: Worker 报告卡顿于 %s\n", (char*)msg->obj);
                free(msg->obj);
                break;
        }
        mq_recycle(&g_looper_mq, msg);
    }

    // 5. 清理与收尾
    // 给所有 Worker 发送 NULL 退出信号
    for (int i = 0; i < num_workers; i++) mq_enqueue(&g_worker_mq, NULL);
    for (int i = 0; i < num_workers; i++) pthread_join(workers[i], NULL);
    free(workers);
    
    mq_destroy(&g_worker_mq);
    mq_destroy(&g_looper_mq);
    
    if (g_visited_history) {
        hash_set_destroy(g_visited_history);
        g_visited_history = NULL;
    }
}

// ==========================================
// 4. 对外接口
// ==========================================

void traverse_files(const Config *cfg, RuntimeState *state) {
    // 1. 初始化异步 Writer
    AsyncWorker *worker = async_worker_init(cfg, state);
    
    // 2. 启动状态监控线程
    ThreadSharedState shared = {
        .cfg = cfg, .state = state, .worker = worker, .running = 1
    };
    pthread_t status_thread;
    pthread_create(&status_thread, NULL, status_thread_func, &shared);

    // 3. 运行主循环 (阻塞直到任务完成)
    run_main_looper(cfg, state, worker);

    // 4. 结束与清理
    async_worker_shutdown(worker);
    shared.running = 0;
    pthread_join(status_thread, NULL);
    
    // 5. 最终归档与清理
    if (cfg->continue_mode) {
        finalize_progress(cfg, state);
        cleanup_progress(cfg, state); // 现在 cleanup 会根据 clean/archive 参数安全删除了
    }
    cleanup_cache(state);
}

// 对外暴露的接口，用于在 progress.c 中增加 pending 计数 (当重放任务时)
void traversal_add_pending_tasks(int count) {
    atomic_fetch_add(&g_pending_tasks, count);
}
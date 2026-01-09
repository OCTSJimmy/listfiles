#include "traversal.h"
#include "looper.h"
#include "utils.h"
#include "async_worker.h"
#include "progress.h"
#include "idempotency.h"
#include "signals.h"
#include "output.h"
#include "monitor.h"        // [新增] 接入 Monitor
#include "device_manager.h" // [新增] 接入 DeviceManager
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

// ==========================================
// 全局队列与状态
// ==========================================
static MessageQueue g_looper_mq;
static MessageQueue g_worker_mq;
static atomic_long g_pending_tasks = 0;

extern HashSet* g_visited_history; 
extern HashSet* g_reference_history;



// ==========================================
// 1. Worker 线程逻辑 (执行者)
// ==========================================

// [修改] 增加心跳指针参数
static void worker_scan_dir(const Config *cfg, const char *dir_path, WorkerHeartbeat *hb) {
    // [心跳] 1. 记录当前意图 (Monitor 探针将使用此路径)
    if (hb) strncpy(hb->current_path, dir_path, sizeof(hb->current_path) - 1);

    // [心跳] 2. opendir 可能会卡在网络 IO
    DIR *dir = opendir(dir_path);
    if (hb) hb->last_active = time(NULL); // 活着回来了

    if (!dir) {
        return;
    }

    struct stat dir_stat;
    dev_t current_dev = 0;
    
    // [心跳] 3. lstat 也可能卡
    if (lstat(dir_path, &dir_stat) == 0) {
        current_dev = dir_stat.st_dev;
        // [心跳] 4. 更新当前设备 (供 Monitor 归类)
        if (hb) hb->current_dev = current_dev;
    }
    if (hb) hb->last_active = time(NULL);

    TaskBatch *batch = batch_create();
    struct dirent *entry;
    time_t now = time(NULL);

    while ((entry = readdir(dir)) != NULL) {
        // [自杀检查] 防止在包含数百万文件的目录中卡死太久
        if (hb && hb->is_zombie) {
            batch_destroy(batch);
            closedir(dir);
            return; // 立即返回，外层会处理退出
        }
        // [心跳] 避免大目录扫描期间被误判
        if (hb) hb->last_active = time(NULL);

        if (entry->d_name[0] == '.' && 
           (entry->d_name[1] == '\0' || (entry->d_name[1] == '.' && entry->d_name[2] == '\0'))) {
            continue;
        }

        char full_path[MAX_PATH_LENGTH];
        int n = snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);
        if (n >= MAX_PATH_LENGTH) continue;

        // === 半增量跳过逻辑 ===
        bool skip_lstat = false;
        struct stat cached_info = {0};

        if (cfg->skip_interval > 0 && g_reference_history && entry->d_ino != 0) {
            HashSetNode *node = hash_set_lookup(g_reference_history, current_dev, entry->d_ino);
            if (node) {
                uint32_t name_hash = calculate_name_hash(entry->d_name);
                bool hash_match = (node->id.name_hash == name_hash);
                bool type_match = true;
                if (entry->d_type != DT_UNKNOWN) {
                    type_match = (node->id.d_type == entry->d_type);
                }
                if (hash_match && type_match) {
                    double age = difftime(now, node->id.mtime);
                    if (age > cfg->skip_interval) {
                        skip_lstat = true;
                        cached_info.st_ino = entry->d_ino;
                        cached_info.st_dev = current_dev;
                        cached_info.st_mtime = node->id.mtime;
                        cached_info.st_atime = node->id.mtime; 
                        cached_info.st_ctime = node->id.mtime; 
                        if (node->id.d_type == DT_DIR) cached_info.st_mode = S_IFDIR | 0755;
                        else if (node->id.d_type == DT_REG) cached_info.st_mode = S_IFREG | 0644;
                        else if (node->id.d_type == DT_LNK) cached_info.st_mode = S_IFLNK | 0777;
                        else cached_info.st_mode = 0;
                        cached_info.st_size = 0; 
                    }
                }
            }
        }

        if (skip_lstat) {
            batch_add(batch, full_path, &cached_info);
        } else {
            struct stat info;
            // [心跳] lstat 是高危操作
            if (lstat(full_path, &info) == 0) {
                batch_add(batch, full_path, &info);
            }
            if (hb) hb->last_active = time(NULL);
        }

        if (batch->count >= BATCH_SIZE) {
            mq_send(&g_looper_mq, MSG_RESULT_BATCH, batch);
            batch = batch_create();
        }
    }
    
    if (batch->count > 0) mq_send(&g_looper_mq, MSG_RESULT_BATCH, batch);
    else batch_destroy(batch);

    closedir(dir);
}

// 批量检查 (用于 Resume 阶段验证文件是否存在)
static void worker_check_batch(TaskBatch *input_batch) {
    TaskBatch *result_batch = batch_create();
    for (int i = 0; i < input_batch->count; i++) {
        struct stat info;
        if (lstat(input_batch->paths[i], &info) == 0) {
            batch_add(result_batch, input_batch->paths[i], &info);
        }
    }
    if (result_batch->count > 0) mq_send(&g_looper_mq, MSG_RESULT_BATCH, result_batch);
    else batch_destroy(result_batch);
    batch_destroy(input_batch);
}

// [修改] Worker 线程入口
static void* worker_thread_entry(void *arg) {
    WorkerArgs *args = (WorkerArgs*)arg;
    const Config *cfg = args->cfg;
    Monitor *monitor = args->monitor;
    free(args); 

    // 1. 向 Monitor 注册 (领身份证)
    WorkerHeartbeat *hb = monitor_register_worker(monitor, pthread_self());

    while (true) {
        // [自杀检查]
        if (hb && hb->is_zombie) {
            goto suicide;
        }
        // === [核心修复] ===
        // 在进入可能无限阻塞的等待之前，清除设备状态。
        // 这样 Monitor 巡检时发现 dev == 0，就会跳过对该 Worker 的超时检查。
        if (hb) {
            hb->current_dev = 0;
            hb->current_path[0] = '\0'; // 可选，清空路径方便调试
        }
        Message *msg = mq_dequeue(&g_worker_mq);
        if (!msg) break; // Quit

        // [心跳] 拿到任务，更新
        if (hb) hb->last_active = time(NULL);
        switch (msg->what) {
            case MSG_SCAN_DIR:
                worker_scan_dir(cfg, (char *)msg->obj, hb);
                free(msg->obj);
                mq_send(&g_looper_mq, MSG_TASK_DONE, NULL);
                break;
                
            case MSG_CHECK_BATCH:
                worker_check_batch((TaskBatch *)msg->obj);
                mq_send(&g_looper_mq, MSG_TASK_DONE, NULL);
                break;
        }
        
        // [心跳] 任务完成，更新
        if (hb) hb->last_active = time(NULL);
        
        mq_recycle(&g_worker_mq, msg);
    }
    
    // 正常退出：注销自己
    monitor_unregister_worker(monitor, hb);
    return NULL;

suicide:
    // 僵尸退出：不注销，直接消失（Monitor 已经把我们移除了）
    // 不需要释放 hb，Monitor 会处理或它已是泄露的代价
    return NULL;
}

// 补位接口
void traversal_spawn_replacement_worker(const Config *cfg, Monitor *monitor) {
    pthread_t tid;
    WorkerArgs *args = safe_malloc(sizeof(WorkerArgs));
    args->cfg = cfg;
    args->monitor = monitor;
    
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    
    if (pthread_create(&tid, &attr, worker_thread_entry, args) == 0) {
        // Log info
    } else {
        free(args);
    }
    pthread_attr_destroy(&attr);
}
// [新增] 通知：Worker 已被遗弃
// 由 monitor.c 调用。当判定一个 Worker 死亡时，必须手动扣除它持有的 pending 任务，
// 否则 Looper 会一直等待 g_pending_tasks 归零，导致死锁。
void traversal_notify_worker_abandoned(void) {
    atomic_fetch_sub(&g_pending_tasks, 1);
}


// ==========================================
// 2. Resume 线程
// ==========================================

static void *resume_thread_entry(void *arg) {
    ResumeThreadArgs *args = (ResumeThreadArgs *)arg;
    restore_progress(args->cfg, args->target_mq, args->state);
    mq_send(&g_looper_mq, MSG_RESUME_FINISHED, NULL);
    free(args);
    return NULL;
}

// ==========================================
// 3. Looper 主控逻辑
// ==========================================

// [修改] 增加 Monitor 参数
static void run_main_looper(const Config *cfg, RuntimeState *state, AsyncWorker *worker, Monitor *monitor) {
    mq_init(&g_looper_mq);
    mq_init(&g_worker_mq);
    atomic_store(&g_pending_tasks, 0);

    g_visited_history = hash_set_create(HASH_SET_INITIAL_SIZE);

    int num_cores = get_nprocs();
    if (num_cores < 1) num_cores = 4;
    int num_workers = num_cores * 2; 
    
    pthread_t *workers = safe_malloc(sizeof(pthread_t) * num_workers);
    for (int i = 0; i < num_workers; i++) {
        WorkerArgs *args = safe_malloc(sizeof(WorkerArgs));
        args->cfg = cfg;
        args->monitor = monitor;
        pthread_create(&workers[i], NULL, worker_thread_entry, args);
    }
    verbose_printf(cfg, 1, "启动 %d 个 Worker 线程\n", num_workers);

    bool resume_active = false;
    LowPriNode *low_pri_head = NULL;
    LowPriNode *low_pri_tail = NULL;

    if (cfg->continue_mode && !g_reference_history) {
        if (load_progress_index(cfg, state)) {
            ResumeThreadArgs *r_args = safe_malloc(sizeof(ResumeThreadArgs));
            r_args->cfg = cfg;
            r_args->state = state;
            r_args->target_mq = &g_worker_mq;
            pthread_t r_tid;
            pthread_create(&r_tid, NULL, resume_thread_entry, r_args);
            pthread_detach(r_tid); 
            resume_active = true;
        }
    }

    if (!resume_active) {
        struct stat root_info;
        if (lstat(cfg->target_path, &root_info) == 0) {
            if (S_ISDIR(root_info.st_mode)) {
                atomic_fetch_add(&g_pending_tasks, 1);
                mq_send(&g_worker_mq, MSG_SCAN_DIR, strdup(cfg->target_path));
            } else {
                push_write_task_file(worker, cfg->target_path, &root_info);
                state->file_count++;
            }
        }
    }

    while (true) {
        if (atomic_load(&g_pending_tasks) == 0 && 
            g_looper_mq.head == NULL && 
            !resume_active && 
            low_pri_head == NULL) {
            break;
        }

        Message *msg = mq_dequeue(&g_looper_mq);
        if (!msg) break;

        switch (msg->what) {
            case MSG_RESULT_BATCH: {
                TaskBatch *batch = (TaskBatch *)msg->obj;
                TaskBatch *output_batch = batch_create();
                
                for (int i = 0; i < batch->count; i++) {
                    char *path = batch->paths[i];
                    struct stat *st = &batch->stats[i];

                    ObjectIdentifier id = { .st_dev = st->st_dev, .st_ino = st->st_ino };
                    if (hash_set_contains(g_visited_history, &id)) continue; 
                    hash_set_insert(g_visited_history, &id);

                    // 熔断检查
                    if (state->dev_mgr && dev_mgr_is_blacklisted(state->dev_mgr, st->st_dev)) {
                        state->has_error = true;
                        continue; 
                    }

                    if (S_ISDIR(st->st_mode)) {
                        if (resume_active) {
                            LowPriNode *node = safe_malloc(sizeof(LowPriNode));
                            node->path = strdup(path);
                            node->next = NULL;
                            if (low_pri_tail) low_pri_tail->next = node;
                            else low_pri_head = node;
                            low_pri_tail = node;
                        } else {
                            atomic_fetch_add(&g_pending_tasks, 1);
                            mq_send(&g_worker_mq, MSG_SCAN_DIR, strdup(path));
                        }
                        
                        state->dir_count++;
                        if (cfg->include_dir) batch_add(output_batch, path, st);
                        if (cfg->print_dir && state->dir_info_fp) {
                            fprintf(state->dir_info_fp, "%s%s\n", OUTPUT_DIR_PREFIX, path);
                        }
                        if (cfg->continue_mode) record_path(cfg, state, path, st);
                        
                    } else {
                        state->file_count++;
                        batch_add(output_batch, path, st);
                    }
                }
                
                if (output_batch->count > 0) push_write_task_batch(worker, output_batch);
                else batch_destroy(output_batch);
                batch_destroy(batch); 
                break;
            }

            case MSG_TASK_DONE:
                atomic_fetch_sub(&g_pending_tasks, 1);
                state->total_dequeued_count++;
                break;

            case MSG_RESUME_FINISHED:
                resume_active = false;
                LowPriNode *curr = low_pri_head;
                while (curr) {
                    atomic_fetch_add(&g_pending_tasks, 1);
                    mq_send(&g_worker_mq, MSG_SCAN_DIR, curr->path); 
                    LowPriNode *next = curr->next;
                    free(curr);
                    curr = next;
                }
                low_pri_head = low_pri_tail = NULL;
                break;
                
            case MSG_WORKER_STUCK:
                free(msg->obj);
                break;
        }
        mq_recycle(&g_looper_mq, msg);
    }

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
    AsyncWorker *worker = async_worker_init(cfg, state);
    Monitor *monitor = monitor_create(cfg, state);
    
    pthread_t monitor_tid;
    pthread_create(&monitor_tid, NULL, monitor_thread_entry, monitor);

    run_main_looper(cfg, state, worker, monitor);

    monitor_destroy(monitor);
    pthread_join(monitor_tid, NULL);

    async_worker_shutdown(worker);
    
    if (cfg->continue_mode) {
        finalize_progress(cfg, state);
        cleanup_progress(cfg, state);
    }
    cleanup_cache(state);
}

void traversal_add_pending_tasks(int count) {
    atomic_fetch_add(&g_pending_tasks, count);
}
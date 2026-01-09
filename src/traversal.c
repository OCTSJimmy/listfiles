#include "traversal.h"
#include "looper.h"
#include "utils.h"
#include "async_worker.h"
#include "progress.h"
#include "idempotency.h"
#include "signals.h"
#include "output.h"
#include "monitor.h"
#include "device_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/sysinfo.h>
#include <errno.h>

// 全局变量
static MessageQueue g_looper_mq;
static MessageQueue g_worker_mq;
static atomic_long g_pending_tasks = 0;

extern HashSet* g_visited_history; 
extern HashSet* g_reference_history;


// Worker 逻辑
static void worker_scan_dir(const Config *cfg, const char *dir_path, WorkerHeartbeat *hb) {
    // 1. 记录意图
    if (hb) strncpy(hb->current_path, dir_path, sizeof(hb->current_path) - 1);
    
    verbose_printf(cfg, 0, "[Worker %d] Scanning dir: %s\n", hb ? hb->id : 0, dir_path);

    DIR *dir = opendir(dir_path);
    if (hb) hb->last_active = time(NULL);

    if (!dir) {
        verbose_printf(cfg, 0, "[Worker %d] Failed to open dir: %s\n", hb ? hb->id : 0, dir_path);
        return;
    }

    struct stat dir_stat;
    dev_t current_dev = 0;
    if (lstat(dir_path, &dir_stat) == 0) {
        current_dev = dir_stat.st_dev;
        if (hb) hb->current_dev = current_dev;
    }
    if (hb) hb->last_active = time(NULL);

    TaskBatch *batch = batch_create();
    struct dirent *entry;

 while ((entry = readdir(dir)) != NULL) {
        if (hb && hb->is_zombie) {
            verbose_printf(cfg, 0, "[Worker %d] ZOMBIE SUICIDE inside readdir loop\n", hb->id);
            batch_destroy(batch);
            closedir(dir);
            return; 
        }
        if (hb) hb->last_active = time(NULL);

        if (entry->d_name[0] == '.' && 
           (entry->d_name[1] == '\0' || (entry->d_name[1] == '.' && entry->d_name[2] == '\0'))) {
            continue;
        }

        char full_path[MAX_PATH_LENGTH];
        int n = snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);
        if (n >= MAX_PATH_LENGTH) continue;

        // ... [省略半增量逻辑，保持原样] ...
        // 假设正常逻辑:
        struct stat info;
        if (lstat(full_path, &info) == 0) {
            batch_add(batch, full_path, &info);
        }
        if (hb) hb->last_active = time(NULL);

        if (batch->count >= BATCH_SIZE) {
            verbose_printf(cfg, 0, "[Worker %d] Sending intermediate batch (%d items)\n", hb->id, batch->count);
            mq_send(&g_looper_mq, MSG_RESULT_BATCH, batch);
            batch = batch_create();
        }
    }
    
    if (batch->count > 0) {
        verbose_printf(cfg, 0, "[Worker %d] Sending final batch (%d items)\n", hb->id, batch->count);
        mq_send(&g_looper_mq, MSG_RESULT_BATCH, batch);
    } else {
        batch_destroy(batch);
    }

    closedir(dir);
    verbose_printf(cfg, 0, "[Worker %d] Finished dir: %s\n", hb ? hb->id : 0, dir_path);
}

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

static void* worker_thread_entry(void *arg) {
    WorkerArgs *args = (WorkerArgs*)arg;
    const Config *cfg = args->cfg;
    Monitor *monitor = args->monitor;
    free(args); 

    WorkerHeartbeat *hb = monitor_register_worker(monitor, pthread_self());
    verbose_printf(cfg, 0, "[Worker %d] Started\n", hb ? hb->id : -1);

    while (true) {
        if (hb && hb->is_zombie) {
            verbose_printf(cfg, 0, "[Worker %d] Zombie detected, suicide.\n", hb->id);
            goto suicide;
        }

        if (hb) {
            hb->current_dev = 0;
            hb->current_path[0] = '\0';
        }

        Message *msg = mq_dequeue(&g_worker_mq);
        
        // [修复] 处理 NULL (队列销毁) 或 MSG_STOP (主动停止)
        if (!msg || msg->what == MSG_STOP) {
            verbose_printf(cfg, 0, "[Worker %d] Received STOP signal\n", hb ? hb->id : -1);
            if (msg) mq_recycle(&g_worker_mq, msg);
            break;
        }

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
        
        if (hb) hb->last_active = time(NULL);
        mq_recycle(&g_worker_mq, msg);
    }
    
    verbose_printf(cfg, 0, "[Worker %d] Exiting normally\n", hb ? hb->id : -1);
    monitor_unregister_worker(monitor, hb);
    return NULL;

suicide:
    return NULL;
}

void traversal_spawn_replacement_worker(const Config *cfg, Monitor *monitor) {
    pthread_t tid;
    WorkerArgs *args = safe_malloc(sizeof(WorkerArgs));
    args->cfg = cfg;
    args->monitor = monitor;
    
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    
    verbose_printf(cfg, 0, "[System] Spawning replacement worker...\n");
    if (pthread_create(&tid, &attr, worker_thread_entry, args) != 0) {
        free(args);
    }
    pthread_attr_destroy(&attr);
}

void traversal_notify_worker_abandoned(void) {
    atomic_fetch_sub(&g_pending_tasks, 1);
}

// Resume Thread
static void *resume_thread_entry(void *arg) {
    ResumeThreadArgs *args = (ResumeThreadArgs *)arg;
    verbose_printf(args->cfg, 0, "[Resume] Thread started\n");
    restore_progress(args->cfg, args->target_mq, args->state);
    verbose_printf(args->cfg, 0, "[Resume] Finished, sending MSG_RESUME_FINISHED\n");
    mq_send(&g_looper_mq, MSG_RESUME_FINISHED, NULL);
    free(args);
    return NULL;
}

// Looper
void run_main_looper(const Config *cfg, RuntimeState *state, AsyncWorker *worker, Monitor *monitor) {
    mq_init(&g_looper_mq);
    mq_init(&g_worker_mq);
    atomic_store(&g_pending_tasks, 0);

    g_visited_history = hash_set_create(HASH_SET_INITIAL_SIZE);

    int num_cores = get_nprocs();
    if (num_cores < 1) num_cores = 4;
    int num_workers = num_cores * 2; 
    
    verbose_printf(cfg, 0, "[Looper] Starting %d workers...\n", num_workers);
    pthread_t *workers = safe_malloc(sizeof(pthread_t) * num_workers);
    for (int i = 0; i < num_workers; i++) {
        WorkerArgs *args = safe_malloc(sizeof(WorkerArgs));
        args->cfg = cfg;
        args->monitor = monitor;
        pthread_create(&workers[i], NULL, worker_thread_entry, args);
    }

    bool resume_active = false;
    LowPriNode *low_pri_head = NULL;
    LowPriNode *low_pri_tail = NULL;

    if (cfg->continue_mode && !g_reference_history) {
        if (load_progress_index(cfg, state)) {
            verbose_printf(cfg, 0, "[Looper] Starting resume thread...\n");
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
        verbose_printf(cfg, 0, "[Looper] Seeding root task: %s\n", cfg->target_path);
        struct stat root_info;
        if (lstat(cfg->target_path, &root_info) == 0) {
            if (S_ISDIR(root_info.st_mode)) {
                atomic_fetch_add(&g_pending_tasks, 1);
                mq_send(&g_worker_mq, MSG_SCAN_DIR, strdup(cfg->target_path));
            } else {
                push_write_task_file(worker, cfg->target_path, &root_info);
                state->file_count++;
            }
        } else {
            fprintf(stderr, "Fatal: Cannot access target path %s\n", cfg->target_path);
        }
    }

    while (true) {
        if (atomic_load(&g_pending_tasks) == 0 && 
            g_looper_mq.head == NULL && 
            !resume_active && 
            low_pri_head == NULL) {
            verbose_printf(cfg, 0, "[Looper] All tasks done. Exiting loop.\n");
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
                        if (cfg->print_dir && state->dir_info_fp) fprintf(state->dir_info_fp, "%s%s\n", OUTPUT_DIR_PREFIX, path);
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
                verbose_printf(cfg, 0, "[Looper] Task Done. Remaining Pending: %ld\n", atomic_load(&g_pending_tasks));
                break;

            case MSG_RESUME_FINISHED:
                verbose_printf(cfg, 0, "[Looper] Resume finished. Flushing low pri queue...\n");
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
        }
        mq_recycle(&g_looper_mq, msg);
    }

    verbose_printf(cfg, 0, "[Looper] Shutting down workers...\n");
    // [核心修复] 使用 MSG_STOP 替代 NULL
    for (int i = 0; i < num_workers; i++) mq_send(&g_worker_mq, MSG_STOP, NULL);
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
    
    // if (cfg->continue_mode) {
    //     finalize_progress(cfg, state);
    //     cleanup_progress(cfg, state);
    // }
    cleanup_cache(state);
}

void traversal_add_pending_tasks(int count) {
    atomic_fetch_add(&g_pending_tasks, count);
}
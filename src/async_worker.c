#include "async_worker.h"
#include "utils.h"
#include "output.h"
#include "progress.h"
#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h> 
#include <time.h>     
#include <errno.h>

// 辅助：执行刷盘
static void perform_flush_output(AsyncWorker *worker) {
    if (worker->pending_count > 0) {
        FILE *fp = worker->state->output_fp ? worker->state->output_fp : stdout;
        fflush(fp);
        worker->pending_count = 0;
        worker->last_flush_time = time(NULL);
    }
}

// 辅助：执行进度保存 (原子更新)
static void perform_save_progress(AsyncWorker *worker, const ProgressSnapshot *snap) {
    char *idx_file = get_index_filename(worker->cfg->progress_base);
    char *tmp_file = safe_malloc(strlen(idx_file) + 32);
    // 加上线程ID防止冲突
    snprintf(tmp_file, strlen(idx_file) + 32, "%s.tmp.%lu", idx_file, (unsigned long)pthread_self());
    
    FILE *tmp_fp = fopen(tmp_file, "w");
    if (tmp_fp) {
        fprintf(tmp_fp, "%lu %lu %lu %lu %lu\n", 
                snap->process_slice_index, 
                snap->processed_count,
                snap->write_slice_index,
                snap->output_slice_num,
                snap->output_line_count);
        fclose(tmp_fp);
        
        if (rename(tmp_file, idx_file) != 0) {
            perror("Worker: 无法更新进度索引");
            unlink(tmp_file);
        }
    }
    free(idx_file);
    free(tmp_file);
}

static void *worker_thread_func(void *arg) {
    // 1. 获取上下文
    AsyncWorker *worker = (AsyncWorker *)arg;
    
    worker->last_flush_time = time(NULL);

    while (true) {
        WriteNode *writeNode = NULL;
        
        // 2. 加锁访问 worker->mutex
        pthread_mutex_lock(&worker->mutex);
        
        while (worker->head == NULL && !worker->stop_flag) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            time_t next_flush = worker->last_flush_time + FLUSH_INTERVAL_SEC;
            time_t now = time(NULL);
            if (next_flush <= now) break; 
            ts.tv_sec = next_flush;
            
            int rc = pthread_cond_timedwait(&worker->cond, &worker->mutex, &ts);
            if (rc == ETIMEDOUT) break;
        }

        if (worker->head != NULL) {
            writeNode = worker->head;
            worker->head = writeNode->next;
            if (worker->head == NULL) worker->tail = NULL;
            worker->queue_count--; 
        } else if (worker->stop_flag) {
            pthread_mutex_unlock(&worker->mutex);
            break; 
        }
        pthread_mutex_unlock(&worker->mutex);

        // 3. 处理业务逻辑 (访问 worker->cfg, worker->state)
        if (writeNode) {
            if (writeNode->type == NODE_TYPE_FILE) {
                // === 处理文件 ===
                struct stat info;
                bool stat_success = false;
                
                // 只有需要元数据时才 lstat
                if (worker->cfg->size || worker->cfg->user || worker->cfg->mtime || 
                    worker->cfg->group || worker->cfg->atime || worker->cfg->format) {
                    
                    if (writeNode->has_cached_stat) {
                        info = writeNode->cached_stat; // 直接用缓存
                        stat_success = true;           
                    } else {
                        if (lstat(writeNode->path, &info) == 0) stat_success = true;
                    }
                } else {
                    memset(&info, 0, sizeof(info));
                    stat_success = true; 
                }
                
                if (stat_success) {
                    worker->state->file_count++; 
                    // 注意：format_output 内部可能还是有锁，但这在这一步暂不处理
                    format_output(worker->cfg, worker->state, writeNode->path, &info);
                    worker->pending_count++;
                }
                free(writeNode->path);

            } else if (writeNode->type == NODE_TYPE_CHECKPOINT) {
                // === 处理检查点 ===
                perform_flush_output(worker);
                perform_save_progress(worker, &writeNode->progress);
                if (writeNode->path) {
                    free(writeNode->path);
                }
            }
            free(writeNode);
        }

        // 4. 自动刷盘逻辑
        time_t now = time(NULL);
        if (worker->pending_count >= BATCH_FLUSH_SIZE || 
           (worker->pending_count > 0 && now - worker->last_flush_time >= FLUSH_INTERVAL_SEC)) {
            perform_flush_output(worker);
        }
    }
    
    perform_flush_output(worker);
    return NULL;
}

AsyncWorker* async_worker_init(const Config *cfg, RuntimeState *state) {
    AsyncWorker *worker = safe_malloc(sizeof(AsyncWorker));
    
    // 初始化成员
    worker->head = NULL;
    worker->tail = NULL;
    worker->stop_flag = false;
    worker->pending_count = 0;
    worker->queue_count = 0;
    worker->last_flush_time = time(NULL);
    worker->cfg = cfg;
    worker->state = state;
    
    pthread_mutex_init(&worker->mutex, NULL);
    pthread_cond_init(&worker->cond, NULL);
    
    // 启动线程，传入 worker 指针
    if (pthread_create(&worker->tid, NULL, worker_thread_func, worker) != 0) {
        perror("Failed to create async worker thread");
        free(worker);
        return NULL;
    }
    
    return worker;
}

void push_write_task_file(AsyncWorker *worker, const char *path, const struct stat *info) {
    if (!worker) return;

    WriteNode *writeNode = safe_malloc(sizeof(WriteNode));
    writeNode->type = NODE_TYPE_FILE;
    writeNode->path = strdup(path); 
    writeNode->next = NULL;

    if (info) {
        writeNode->has_cached_stat = true;
        writeNode->cached_stat = *info;
    } else {
        writeNode->has_cached_stat = false;
    }

    pthread_mutex_lock(&worker->mutex);
    if (worker->tail) {
        worker->tail->next = writeNode;
        worker->tail = writeNode;
    } else {
        worker->head = worker->tail = writeNode;
    }
    worker->queue_count++;
    pthread_cond_signal(&worker->cond);
    pthread_mutex_unlock(&worker->mutex);
}

void push_write_task_checkpoint(AsyncWorker *worker, const RuntimeState *current_state) {
    if (!worker) return;

    WriteNode *writeNode = safe_malloc(sizeof(WriteNode));
    writeNode->type = NODE_TYPE_CHECKPOINT;
    writeNode->path = NULL;
    
    writeNode->progress.process_slice_index = current_state->process_slice_index;
    writeNode->progress.processed_count     = current_state->processed_count;
    writeNode->progress.write_slice_index   = current_state->write_slice_index;
    writeNode->progress.output_slice_num    = current_state->output_slice_num;
    writeNode->progress.output_line_count   = current_state->output_line_count;
    
    writeNode->next = NULL;

    pthread_mutex_lock(&worker->mutex);
    if (worker->tail) {
        worker->tail->next = writeNode;
        worker->tail = writeNode;
    } else {
        worker->head = worker->tail = writeNode;
    }
    worker->queue_count++;
    pthread_cond_signal(&worker->cond);
    pthread_mutex_unlock(&worker->mutex);
}

void async_worker_shutdown(AsyncWorker *worker) {
    if (!worker) return;

    pthread_mutex_lock(&worker->mutex);
    worker->stop_flag = true;
    pthread_cond_signal(&worker->cond);
    pthread_mutex_unlock(&worker->mutex);
    
    if (worker->tid) {
        pthread_join(worker->tid, NULL); 
    }

    // 清理资源
    pthread_mutex_destroy(&worker->mutex);
    pthread_cond_destroy(&worker->cond);
    
    // 清理残留队列（如果有）
    WriteNode *curr = worker->head;
    while(curr) {
        WriteNode *next = curr->next;
        if (curr->path) free(curr->path);
        free(curr);
        curr = next;
    }
    
    free(worker);
}

size_t async_worker_get_queue_size(AsyncWorker *worker) {
    if (!worker) return 0;
    pthread_mutex_lock(&worker->mutex);
    size_t size = worker->queue_count;
    pthread_mutex_unlock(&worker->mutex);
    return size;
}
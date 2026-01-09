#include "async_worker.h"
#include "utils.h"
#include "output.h"
#include "progress.h" // 需要 get_index_filename
#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdio.h>

#define FLUSH_INTERVAL_SEC 5
#define BATCH_FLUSH_THRESHOLD 5000 

// ==========================================
// 辅助函数
// ==========================================

// 处理单个文件输出
static void process_single_file_output(AsyncWorker *worker, const char *path, const struct stat *st) {
    const Config *cfg = worker->cfg;
    RuntimeState *state = worker->state;
    FILE *target_fp = NULL;

    // 1. 确定输出流 (分片逻辑 or 单文件逻辑)
    if (cfg->is_output_split_dir) {
        // [分片模式]
        if (state->output_line_count >= cfg->output_slice_lines) {
            if (state->output_fp) {
                fclose(state->output_fp);
                state->output_fp = NULL;
            }
            state->output_slice_num++;
            state->output_line_count = 0;
        }

        if (!state->output_fp) {
            char slice_path[1024];
            char filename[64];
            snprintf(filename, sizeof(filename), "%06lu.txt", state->output_slice_num);
            snprintf(slice_path, sizeof(slice_path), "%s/output_%s", cfg->output_split_dir, filename);
            
            state->output_fp = fopen(slice_path, "w");
            if (!state->output_fp) {
                perror("无法创建输出分片"); // 严重错误，暂且打印
                return;
            }
        }
        target_fp = state->output_fp;
        
    } else if (cfg->is_output_file) {
        // [单文件模式]
        if (!state->output_fp) {
            state->output_fp = fopen(cfg->output_file, "a");
            if (!state->output_fp) return;
        }
        target_fp = state->output_fp;
        
    } else {
        target_fp = stdout;
    }

    if (!target_fp) return;

    // 2. 执行流式输出
    print_to_stream(cfg, state, path, st, target_fp);
    state->output_line_count++;
}

// 执行刷盘 (fflush)
static void perform_flush_output(AsyncWorker *worker) {
    FILE *fp = worker->state->output_fp ? worker->state->output_fp : stdout;
    if (fp) fflush(fp);
    worker->pending_since_flush = 0;
    worker->last_flush_time = time(NULL);
}

// 执行进度保存 (原子更新 index 文件)
static void perform_save_progress(AsyncWorker *worker, const ProgressSnapshot *snap) {
    if (!worker->cfg->progress_base) return;

    char *idx_file = get_index_filename(worker->cfg->progress_base);
    char *tmp_file = safe_malloc(strlen(idx_file) + 32);
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
            unlink(tmp_file); // 失败回滚
        }
    }
    free(idx_file);
    free(tmp_file);
}

// ==========================================
// 线程主体
// ==========================================

static void *async_writer_thread(void *arg) {
    AsyncWorker *worker = (AsyncWorker *)arg;
    worker->last_flush_time = time(NULL);
    verbose_printf(worker->cfg, 0, "[Writer] Thread started\n");
    while (true) {
        WriteTask *task = NULL;
        pthread_mutex_lock(&worker->mutex);
        while (worker->head == NULL && !worker->stop_flag) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += FLUSH_INTERVAL_SEC; 
            int rc = pthread_cond_timedwait(&worker->cond, &worker->mutex, &ts);
            if (rc == ETIMEDOUT) break; 
        }

        if (worker->head != NULL) {
            task = worker->head;
            worker->head = task->next;
            if (worker->head == NULL) worker->tail = NULL;
            worker->queue_count--;
        } else if (worker->stop_flag) {
            pthread_mutex_unlock(&worker->mutex);
            break; 
        }
        pthread_mutex_unlock(&worker->mutex);

        if (task) {
            if (task->type == TASK_WRITE_BATCH) {
                TaskBatch *batch = task->data.batch;
                if (batch) {
                    verbose_printf(worker->cfg, 0, "[Writer] Processing batch (%d items)\n", batch->count);
                    for (int i = 0; i < batch->count; i++) {
                        process_single_file_output(worker, batch->paths[i], &batch->stats[i]);
                        worker->pending_since_flush++;
                    }
                    batch_destroy(batch);
                }
            } else if (task->type == TASK_WRITE_CHECKPOINT) {
                verbose_printf(worker->cfg, 0, "[Writer] Checkpoint triggered\n");
                perform_flush_output(worker);
                perform_save_progress(worker, &task->data.checkpoint);
            }
            free(task);
        }

        time_t now = time(NULL);
        if (worker->pending_since_flush >= BATCH_FLUSH_SIZE || 
            (worker->pending_since_flush > 0 && now - worker->last_flush_time >= FLUSH_INTERVAL_SEC)) {
            verbose_printf(worker->cfg, 0, "[Writer] Auto-flushing %zu items...\n", worker->pending_since_flush);
            perform_flush_output(worker);
        }
    }
    
    verbose_printf(worker->cfg, 0, "[Writer] Exiting, final flush...\n");
    perform_flush_output(worker);
    return NULL;
}

// ==========================================
// 公开接口实现
// ==========================================

AsyncWorker *async_worker_init(const Config *cfg, RuntimeState *state) {
    AsyncWorker *worker = safe_malloc(sizeof(AsyncWorker));
    memset(worker, 0, sizeof(AsyncWorker));
    
    worker->cfg = cfg;
    worker->state = state;
    worker->stop_flag = false;
    
    pthread_mutex_init(&worker->mutex, NULL);
    pthread_cond_init(&worker->cond, NULL);
    
    // 初始化文件/目录状态
    if (cfg->is_output_file) {
        FILE *fp = fopen(cfg->output_file, "w"); // Truncate
        if (fp) fclose(fp);
    }
    if (cfg->is_output_split_dir) {
        mkdir(cfg->output_split_dir, 0755);
    }

    pthread_create(&worker->thread, NULL, async_writer_thread, worker);
    return worker;
}

void async_worker_shutdown(AsyncWorker *worker) {
    if (!worker) return;

    pthread_mutex_lock(&worker->mutex);
    worker->stop_flag = true;
    pthread_cond_signal(&worker->cond);
    pthread_mutex_unlock(&worker->mutex);

    pthread_join(worker->thread, NULL);
    
    if (worker->state->output_fp && worker->state->output_fp != stdout) {
        fclose(worker->state->output_fp);
        worker->state->output_fp = NULL;
    }

    pthread_mutex_destroy(&worker->mutex);
    pthread_cond_destroy(&worker->cond);
    
    // 清理剩余任务 (如果是非正常退出)
    while (worker->head) {
        WriteTask *tmp = worker->head;
        worker->head = worker->head->next;
        if (tmp->type == TASK_WRITE_BATCH && tmp->data.batch) {
            batch_destroy(tmp->data.batch);
        }
        free(tmp);
    }
    free(worker);
}

// 内部入队通用函数
static void enqueue_task(AsyncWorker *worker, WriteTask *task) {
    pthread_mutex_lock(&worker->mutex);
    if (worker->tail) {
        worker->tail->next = task;
    } else {
        worker->head = task;
    }
    worker->tail = task;
    worker->queue_count++;
    pthread_cond_signal(&worker->cond);
    pthread_mutex_unlock(&worker->mutex);
}

void push_write_task_batch(AsyncWorker *worker, TaskBatch *batch) {
    if (!worker || !batch) return;
    WriteTask *task = safe_malloc(sizeof(WriteTask));
    task->type = TASK_WRITE_BATCH;
    task->data.batch = batch;
    task->next = NULL;
    enqueue_task(worker, task);
}

void push_write_task_file(AsyncWorker *worker, const char *path, const struct stat *info) {
    // 包装成 Batch 发送，保持内部逻辑统一
    TaskBatch *batch = batch_create();
    batch_add(batch, path, info);
    push_write_task_batch(worker, batch);
}

void push_write_task_checkpoint(AsyncWorker *worker, const RuntimeState *current_state) {
    if (!worker) return;
    WriteTask *task = safe_malloc(sizeof(WriteTask));
    task->type = TASK_WRITE_CHECKPOINT;
    
    // 捕获当前状态快照
    task->data.checkpoint.process_slice_index = current_state->process_slice_index;
    task->data.checkpoint.processed_count = current_state->processed_count;
    task->data.checkpoint.write_slice_index = current_state->write_slice_index;
    task->data.checkpoint.output_slice_num = current_state->output_slice_num;
    task->data.checkpoint.output_line_count = current_state->output_line_count;
    
    task->next = NULL;
    enqueue_task(worker, task);
}

size_t async_worker_get_queue_size(AsyncWorker *worker) {
    if (!worker) return 0;
    pthread_mutex_lock(&worker->mutex);
    size_t size = worker->queue_count;
    pthread_mutex_unlock(&worker->mutex);
    return size;
}
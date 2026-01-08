#ifndef ASYNC_WORKER_H
#define ASYNC_WORKER_H

#include "config.h"
#include "looper.h" // 需要 TaskBatch 定义
#include <pthread.h>
#include <stdbool.h>

// 任务类型枚举
typedef enum {
    TASK_WRITE_BATCH,      // 批量文件写入
    TASK_WRITE_CHECKPOINT, // 进度检查点 (保存 pbin 索引)
    TASK_WRITE_STOP        // 停止信号 (可选，也可以通过 flag 控制)
} WriteTaskType;

// 进度快照 (用于 Checkpoint)
typedef struct {
    unsigned long process_slice_index;
    unsigned long processed_count;
    unsigned long write_slice_index;
    unsigned long output_slice_num;
    unsigned long output_line_count;
} ProgressSnapshot;

// 统一的任务节点
typedef struct WriteTask {
    WriteTaskType type;
    union {
        TaskBatch *batch;            // 当 type == TASK_WRITE_BATCH 时有效
        ProgressSnapshot checkpoint; // 当 type == TASK_WRITE_CHECKPOINT 时有效
    } data;
    struct WriteTask *next;
} WriteTask;

// AsyncWorker 上下文
typedef struct AsyncWorker {
    // 队列头尾
    struct WriteTask *head;
    struct WriteTask *tail;
    
    // 线程同步
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    pthread_t thread;
    
    // 状态控制
    volatile bool stop_flag;
    size_t queue_count;
    time_t last_flush_time;
    unsigned long pending_since_flush; // 距离上次 flush 积压的条数
    
    // 外部引用
    const Config *cfg;
    RuntimeState *state;
} AsyncWorker;

// === 公开接口 ===

AsyncWorker* async_worker_init(const Config *cfg, RuntimeState *state);
void async_worker_shutdown(AsyncWorker *worker);

// 提交批量写入任务
void push_write_task_batch(AsyncWorker *worker, TaskBatch *batch);

// 提交单个文件 (内部会包装成 Batch)
void push_write_task_file(AsyncWorker *worker, const char *path, const struct stat *info);

// 提交检查点 (触发 flush 并保存进度索引)
void push_write_task_checkpoint(AsyncWorker *worker, const RuntimeState *current_state);

// 获取队列积压数
size_t async_worker_get_queue_size(AsyncWorker *worker);

#endif // ASYNC_WORKER_H
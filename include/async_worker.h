#ifndef ASYNC_WORKER_H
#define ASYNC_WORKER_H

#include "config.h"
#include <pthread.h>
#include <stdbool.h>

typedef enum {
    NODE_TYPE_FILE,
    NODE_TYPE_CHECKPOINT
} WriteNodeType;

// 进度快照结构体
typedef struct {
    unsigned long process_slice_index;
    unsigned long processed_count;
    unsigned long write_slice_index;
    unsigned long output_slice_num;
    unsigned long output_line_count;
} ProgressSnapshot;

typedef struct WriteNode {
    WriteNodeType type;
    char *path;               // 仅 TYPE_FILE 有效
    ProgressSnapshot progress; // 仅 TYPE_CHECKPOINT 有效
    struct WriteNode *next;
    bool has_cached_stat;
    struct stat cached_stat;
} WriteNode;

// === 新增：AsyncWorker 上下文结构体 ===
// 不再依赖全局变量，所有状态封装在此
typedef struct AsyncWorker {
    struct WriteNode *head;
    struct WriteNode *tail;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    bool stop_flag;
    unsigned long pending_count;
    size_t queue_count;
    time_t last_flush_time;
    
    // 引用外部配置和状态
    const Config *cfg;
    RuntimeState *state;
    
    pthread_t tid;
} AsyncWorker;

// === 修改后的公开接口 ===

// 初始化异步工作线程，返回实例指针
AsyncWorker* async_worker_init(const Config *cfg, RuntimeState *state);

// 提交一个文件路径 (主线程调用，需传入 worker 实例)
void push_write_task_file(AsyncWorker *worker, const char *path, const struct stat *info);

// 提交一个进度检查点 (主线程调用，需传入 worker 实例)
void push_write_task_checkpoint(AsyncWorker *worker, const RuntimeState *current_state);

// 等待处理完毕并关闭，同时释放 worker 内存
void async_worker_shutdown(AsyncWorker *worker);

// 获取队列大小
size_t async_worker_get_queue_size(AsyncWorker *worker);

#endif
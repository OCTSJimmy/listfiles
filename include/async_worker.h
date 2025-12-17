#ifndef ASYNC_WORKER_H
#define ASYNC_WORKER_H

#include "config.h"
#include "config.h"
#include "looper.h" // [新增] 需要 TaskBatch 定义
#include <pthread.h>
#include <stdbool.h>

typedef enum {
    NODE_TYPE_FILE,
    NODE_TYPE_CHECKPOINT,
    NODE_TYPE_BATCH
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
    
    // 联合体 payload (可选优化，为了代码清晰暂时维持原样或直接添加字段)
    // 为保持兼容性，我们直接添加字段，依靠 type 区分
    char *path;               
    
    // [新增] 批量数据包
    TaskBatch *batch;         

    ProgressSnapshot progress; 
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

// [新增] 提交一批文件任务 (worker 将接管 batch 的内存所有权)
void push_write_task_batch(AsyncWorker *worker, TaskBatch *batch);

void push_write_task_checkpoint(AsyncWorker *worker, const RuntimeState *current_state);

void async_worker_shutdown(AsyncWorker *worker);

size_t async_worker_get_queue_size(AsyncWorker *worker);

#endif
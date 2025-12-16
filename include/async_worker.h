#ifndef ASYNC_WORKER_H
#define ASYNC_WORKER_H

#include "config.h"
typedef enum {
    NODE_TYPE_FILE,
    NODE_TYPE_CHECKPOINT
} WriteNodeType;

// 初始化异步工作线程
void async_worker_init(const Config *cfg, RuntimeState *state);

// 提交一个文件路径 (主线程调用)
void push_write_task_file(const char *path, const struct stat *info);

// 提交一个进度检查点 (主线程调用)
// 这意味着：在此之前的所有文件都必须落盘，然后保存 state 中的进度
void push_write_task_checkpoint(const RuntimeState *current_state);

// 等待处理完毕并关闭
void async_worker_shutdown();
size_t async_worker_get_queue_size();
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


#endif
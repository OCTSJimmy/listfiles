#ifndef ASYNC_WORKER_H
#define ASYNC_WORKER_H

#include "config.h"

// 初始化异步工作线程
void async_worker_init(const Config *cfg, RuntimeState *state);

// 提交一个文件路径到异步队列 (主线程调用)
// 只需要传路径，不需要传 stat info，因为我们延后到 worker 里做
void async_worker_push_file(const char *path);

// 等待所有任务处理完毕并关闭 (主线程调用)
void async_worker_shutdown();
size_t async_worker_get_queue_size();
#endif
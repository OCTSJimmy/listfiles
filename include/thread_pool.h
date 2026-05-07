#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/stat.h>

/* 单个 batch 去重任务 */
typedef struct {
    char **paths;
    struct stat *stats;
    int count;
    uint8_t *results;   /* 输出掩码：bit0=duplicate, bit1=blacklisted */
    int worker_id;
} TPBatch;

typedef void (*tp_process_fn)(TPBatch *batch, void *user_data);

typedef struct ThreadPool ThreadPool;

ThreadPool* thread_pool_create(int num_threads, int event_fd, tp_process_fn fn, void *user_data);
void thread_pool_destroy(ThreadPool *tp);

/* 提交 batch 去重任务。成功返回 true，队列满返回 false（调用方应降级同步处理） */
bool thread_pool_submit(ThreadPool *tp, TPBatch *batch);

/* 主线程调用：取出所有已完成的 batch。返回 NULL 表示没有已完成的 batch。
 * 调用方需负责释放返回的 TPBatch 及其内部内存。 */
TPBatch* thread_pool_poll_completed(ThreadPool *tp);

#endif

#ifndef ASYNC_WORKER_H
#define ASYNC_WORKER_H

#include "config.h"
#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>

#define ASYNC_BATCH_SIZE 256

typedef struct OutputTask {
    char *path;
    struct stat st;
    struct OutputTask *next;
} OutputTask;

typedef struct OutputBatch {
    OutputTask *head;
    OutputTask *tail;
    int count;
    int slot_id;                 /* v15.6.0: 归属 Worker slot（P0-002），-1 表示无任务归属不计数 */
    struct OutputBatch *next;    /* v15.6.0: 输出线程 batch 队列链接 */
} OutputBatch;

typedef struct AsyncWorker {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    pthread_t thread;
    OutputBatch *head;           /* v15.6.0: batch 节点队列（保留批次边界，供按批 fflush/计数） */
    OutputBatch *tail;
    bool stop;
    const Config *cfg;
    RuntimeState *state;
    _Atomic long *output_pending; /* v15.6.0: 每 slot 未 COMMITTED 输出 batch 计数（P0-002，可 NULL） */
    pthread_cond_t *main_cond;    /* v15.6.0: 输出 COMMITTED 后唤醒主线程推进完成屏障（可 NULL） */
} AsyncWorker;

AsyncWorker* async_worker_init(const Config *cfg, RuntimeState *state, _Atomic long *output_pending,
                               pthread_cond_t *main_cond);
void async_worker_shutdown(AsyncWorker *worker);
void async_writer_submit(AsyncWorker *worker, const char *path, const struct stat *st);

/* 批量提交：将 OutputBatch 中所有任务一次性加入队列（仅一次 mutex lock） */
void async_writer_submit_batch(AsyncWorker *worker, OutputBatch *batch);

#endif

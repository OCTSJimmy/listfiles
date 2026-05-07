#ifndef ASYNC_WORKER_H
#define ASYNC_WORKER_H

#include "config.h"
#include <pthread.h>
#include <stdbool.h>

#define ASYNC_BATCH_SIZE 256

typedef struct OutputTask {
    char *path;
    struct stat st;
    struct OutputTask *next;
} OutputTask;

typedef struct {
    OutputTask *head;
    OutputTask *tail;
    int count;
} OutputBatch;

typedef struct AsyncWorker {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    pthread_t thread;
    OutputTask *head;
    OutputTask *tail;
    bool stop;
    const Config *cfg;
    RuntimeState *state;
} AsyncWorker;

AsyncWorker* async_worker_init(const Config *cfg, RuntimeState *state);
void async_worker_shutdown(AsyncWorker *worker);
void async_writer_submit(AsyncWorker *worker, const char *path, const struct stat *st);

/* 批量提交：将 OutputBatch 中所有任务一次性加入队列（仅一次 mutex lock） */
void async_writer_submit_batch(AsyncWorker *worker, OutputBatch *batch);

#endif

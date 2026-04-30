#ifndef ASYNC_WORKER_H
#define ASYNC_WORKER_H

#include "config.h"
#include <pthread.h>
#include <stdbool.h>

typedef struct OutputTask {
    char *path;
    struct stat st;
    struct OutputTask *next;
} OutputTask;

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

#endif

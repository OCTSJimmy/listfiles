#include "async_worker.h"
#include "output.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

static void *async_writer_thread(void *arg) {
    AsyncWorker *w = (AsyncWorker*)arg;
    while (1) {
        pthread_mutex_lock(&w->mutex);
        while (!w->stop && w->head == NULL) {
            pthread_cond_wait(&w->cond, &w->mutex);
        }
        if (w->stop && w->head == NULL) {
            pthread_mutex_unlock(&w->mutex);
            break;
        }
        OutputTask *task = w->head;
        w->head = task->next;
        if (w->head == NULL) w->tail = NULL;
        pthread_mutex_unlock(&w->mutex);

        if (w->state->output_fp) {
            print_to_stream(w->cfg, w->state, task->path, &task->st, w->state->output_fp);
            w->state->output_line_count++;
            if (w->cfg->is_output_split_dir && w->state->output_line_count >= w->cfg->output_slice_lines) {
                rotate_output_slice(w->cfg, w->state);
            }
        }
        free(task->path);
        free(task);
    }
    if (w->state->output_fp && w->state->output_fp != stdout) {
        fflush(w->state->output_fp);
    }
    return NULL;
}

AsyncWorker* async_worker_init(const Config *cfg, RuntimeState *state) {
    AsyncWorker *w = calloc(1, sizeof(AsyncWorker));
    if (!w) return NULL;
    w->cfg = cfg;
    w->state = state;
    pthread_mutex_init(&w->mutex, NULL);
    pthread_cond_init(&w->cond, NULL);
    pthread_create(&w->thread, NULL, async_writer_thread, w);
    return w;
}

void async_worker_shutdown(AsyncWorker *w) {
    if (!w) return;
    pthread_mutex_lock(&w->mutex);
    w->stop = true;
    pthread_cond_signal(&w->cond);
    pthread_mutex_unlock(&w->mutex);
    pthread_join(w->thread, NULL);

    OutputTask *t = w->head;
    while (t) {
        OutputTask *next = t->next;
        free(t->path);
        free(t);
        t = next;
    }
    pthread_mutex_destroy(&w->mutex);
    pthread_cond_destroy(&w->cond);
    free(w);
}

void async_writer_submit(AsyncWorker *w, const char *path, const struct stat *st) {
    if (!w || !path) return;
    OutputTask *task = calloc(1, sizeof(OutputTask));
    if (!task) return;
    task->path = strdup(path);
    task->st = *st;

    pthread_mutex_lock(&w->mutex);
    if (w->tail) {
        w->tail->next = task;
    } else {
        w->head = task;
    }
    w->tail = task;
    pthread_cond_signal(&w->cond);
    pthread_mutex_unlock(&w->mutex);
}

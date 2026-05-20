#include "dispatch_queue.h"
#include <stdlib.h>
#include <string.h>

#define INITIAL_CAPACITY 1024

void dispatch_queue_init(DispatchQueue *q) {
    q->tasks = NULL;
    q->count = 0;
    q->capacity = 0;
    pthread_mutex_init(&q->mutex, NULL);
}

void dispatch_queue_destroy(DispatchQueue *q) {
    if (!q) return;
    for (size_t i = 0; i < q->count; i++) {
        if (q->tasks[i].path) free(q->tasks[i].path);
    }
    free(q->tasks);
    q->tasks = NULL;
    q->count = 0;
    q->capacity = 0;
    pthread_mutex_destroy(&q->mutex);
}

bool dispatch_queue_push(DispatchQueue *q, char *path, const struct stat *st) {
    if (!q || !path) return false;

    pthread_mutex_lock(&q->mutex);

    if (q->count >= q->capacity) {
        size_t new_cap = q->capacity ? q->capacity * 2 : INITIAL_CAPACITY;
        DispatchTask *new_tasks = realloc(q->tasks, new_cap * sizeof(DispatchTask));
        if (!new_tasks) {
            pthread_mutex_unlock(&q->mutex);
            return false;
        }
        q->tasks = new_tasks;
        q->capacity = new_cap;
    }

    q->tasks[q->count].path = path;
    if (st) {
        q->tasks[q->count].st = *st;
    } else {
        memset(&q->tasks[q->count].st, 0, sizeof(struct stat));
    }
    q->count++;

    pthread_mutex_unlock(&q->mutex);
    return true;
}

void dispatch_queue_push_backlog(DispatchQueue *q, char **backlog_paths, struct stat *backlog_stats, int backlog_count) {
    if (!q || !backlog_paths || backlog_count <= 0) return;

    pthread_mutex_lock(&q->mutex);

    size_t needed = q->count + (size_t)backlog_count;
    if (needed > q->capacity) {
        size_t new_cap = q->capacity ? q->capacity * 2 : INITIAL_CAPACITY;
        while (new_cap < needed) new_cap *= 2;
        DispatchTask *new_tasks = realloc(q->tasks, new_cap * sizeof(DispatchTask));
        if (!new_tasks) {
            pthread_mutex_unlock(&q->mutex);
            return;
        }
        q->tasks = new_tasks;
        q->capacity = new_cap;
    }

    for (int i = 0; i < backlog_count; i++) {
        if (backlog_paths[i]) {
            q->tasks[q->count].path = backlog_paths[i];
            if (backlog_stats) {
                q->tasks[q->count].st = backlog_stats[i];
            } else {
                memset(&q->tasks[q->count].st, 0, sizeof(struct stat));
            }
            q->count++;
            backlog_paths[i] = NULL;
        }
    }

    pthread_mutex_unlock(&q->mutex);
}

bool dispatch_queue_pop(DispatchQueue *q, DispatchTask *task_out) {
    if (!q || !task_out) return false;

    pthread_mutex_lock(&q->mutex);

    if (q->count == 0) {
        pthread_mutex_unlock(&q->mutex);
        return false;
    }

    *task_out = q->tasks[0];

    /* shift remaining elements */
    memmove(&q->tasks[0], &q->tasks[1], (q->count - 1) * sizeof(DispatchTask));
    q->count--;

    pthread_mutex_unlock(&q->mutex);
    return true;
}

void dispatch_queue_compact(DispatchQueue *q) {
    if (!q) return;

    pthread_mutex_lock(&q->mutex);

    size_t write_idx = 0;
    for (size_t read_idx = 0; read_idx < q->count; read_idx++) {
        if (q->tasks[read_idx].path != NULL) {
            if (write_idx != read_idx) {
                q->tasks[write_idx] = q->tasks[read_idx];
            }
            write_idx++;
        }
    }
    q->count = write_idx;

    pthread_mutex_unlock(&q->mutex);
}

size_t dispatch_queue_count(DispatchQueue *q) {
    if (!q) return 0;

    pthread_mutex_lock(&q->mutex);
    size_t count = q->count;
    pthread_mutex_unlock(&q->mutex);
    return count;
}

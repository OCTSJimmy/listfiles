#include "dispatch_queue.h"
#include <stdlib.h>
#include <string.h>

#define INITIAL_CAPACITY 1024

static inline size_t ring_idx(const DispatchQueue *q, size_t i) {
    return (q->head + i) % q->capacity;
}

/* Compact ring buffer to linear [0, count) before realloc or when fragmented */
static void dispatch_queue_linearize(DispatchQueue *q) {
    if (q->count == 0 || q->head == 0) {
        q->head = 0;
        return;
    }
    if (q->head + q->count <= q->capacity) {
        /* Contiguous wrapped segment: [head, head+count) → [0, count) */
        memmove(&q->tasks[0], &q->tasks[q->head], q->count * sizeof(DispatchTask));
    } else {
        /* Split into two segments: [head, cap) and [0, tail) */
        size_t first_len = q->capacity - q->head;
        size_t second_len = q->count - first_len;
        DispatchTask *tmp = malloc(q->count * sizeof(DispatchTask));
        if (!tmp) {
            /* Fallback: keep ring as-is, next push may still work if count < capacity */
            return;
        }
        memcpy(tmp, &q->tasks[q->head], first_len * sizeof(DispatchTask));
        memcpy(tmp + first_len, &q->tasks[0], second_len * sizeof(DispatchTask));
        memcpy(q->tasks, tmp, q->count * sizeof(DispatchTask));
        free(tmp);
    }
    q->head = 0;
}

void dispatch_queue_init(DispatchQueue *q) {
    q->tasks = NULL;
    q->count = 0;
    q->capacity = 0;
    q->head = 0;
    pthread_mutex_init(&q->mutex, NULL);
}

void dispatch_queue_destroy(DispatchQueue *q) {
    if (!q) return;
    for (size_t i = 0; i < q->count; i++) {
        size_t idx = ring_idx(q, i);
        if (q->tasks[idx].path) free(q->tasks[idx].path);
    }
    free(q->tasks);
    q->tasks = NULL;
    q->count = 0;
    q->capacity = 0;
    q->head = 0;
    pthread_mutex_destroy(&q->mutex);
}

bool dispatch_queue_push(DispatchQueue *q, char *path, const struct stat *st) {
    if (!q || !path) return false;

    pthread_mutex_lock(&q->mutex);

    if (q->count >= q->capacity) {
        if (q->head > 0 && q->count > 0) {
            dispatch_queue_linearize(q);
        }
        size_t new_cap = q->capacity ? q->capacity * 2 : INITIAL_CAPACITY;
        DispatchTask *new_tasks = realloc(q->tasks, new_cap * sizeof(DispatchTask));
        if (!new_tasks) {
            pthread_mutex_unlock(&q->mutex);
            return false;
        }
        q->tasks = new_tasks;
        q->capacity = new_cap;
    }

    size_t idx = ring_idx(q, q->count);
    q->tasks[idx].path = path;
    if (st) {
        q->tasks[idx].st = *st;
    } else {
        memset(&q->tasks[idx].st, 0, sizeof(struct stat));
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
        if (q->head > 0 && q->count > 0) {
            dispatch_queue_linearize(q);
        }
        size_t new_cap = q->capacity ? q->capacity * 2 : INITIAL_CAPACITY;
        while (new_cap < needed) new_cap *= 2;
        DispatchTask *new_tasks = realloc(q->tasks, new_cap * sizeof(DispatchTask));
        if (!new_tasks) {
            for (int i = 0; i < backlog_count; i++) {
                free(backlog_paths[i]);
                backlog_paths[i] = NULL;
            }
            pthread_mutex_unlock(&q->mutex);
            return;
        }
        q->tasks = new_tasks;
        q->capacity = new_cap;
    }

    for (int i = 0; i < backlog_count; i++) {
        if (backlog_paths[i]) {
            size_t idx = ring_idx(q, q->count);
            q->tasks[idx].path = backlog_paths[i];
            if (backlog_stats) {
                q->tasks[idx].st = backlog_stats[i];
            } else {
                memset(&q->tasks[idx].st, 0, sizeof(struct stat));
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

    size_t idx = q->head;
    *task_out = q->tasks[idx];
    q->tasks[idx].path = NULL;  /* defensive */
    q->head = (q->head + 1) % (q->capacity ? q->capacity : 1);
    q->count--;

    pthread_mutex_unlock(&q->mutex);
    return true;
}

void dispatch_queue_compact(DispatchQueue *q) {
    if (!q) return;
    pthread_mutex_lock(&q->mutex);
    if (q->head > 0 && q->count > 0) {
        dispatch_queue_linearize(q);
    }
    pthread_mutex_unlock(&q->mutex);
}

size_t dispatch_queue_count(DispatchQueue *q) {
    if (!q) return 0;
    pthread_mutex_lock(&q->mutex);
    size_t c = q->count;
    pthread_mutex_unlock(&q->mutex);
    return c;
}

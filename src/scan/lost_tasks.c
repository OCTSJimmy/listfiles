/**
 * @file lost_tasks.c
 * @brief LostTasksQueue 实现 —— 封装 lost_tasks 的数据 + 锁
 *
 * 所有锁操作隐藏在本模块内部，外部通过 API 访问。
 */
#include "lost_tasks.h"
#include <stdlib.h>
#include <stdio.h>
#include "log.h"

void lost_tasks_init(LostTasksQueue *q) {
    if (!q) return;
    q->tasks = NULL;
    q->count = 0;
    q->capacity = 0;
    pthread_mutex_init(&q->mutex, NULL);
}

void lost_tasks_destroy(LostTasksQueue *q) {
    if (!q) return;
    pthread_mutex_lock(&q->mutex);
    for (size_t i = 0; i < q->count; i++) {
        free(q->tasks[i]);
    }
    free(q->tasks);
    q->tasks = NULL;
    q->count = 0;
    q->capacity = 0;
    pthread_mutex_unlock(&q->mutex);
    pthread_mutex_destroy(&q->mutex);
}

bool lost_tasks_push(LostTasksQueue *q, char *path) {
    if (!q || !path) return false;
    pthread_mutex_lock(&q->mutex);
    if (q->count >= q->capacity) {
        size_t new_cap = q->capacity ? q->capacity * 2 : 64;
        char **new_arr = realloc(q->tasks, new_cap * sizeof(char *));
        if (!new_arr) {
            pthread_mutex_unlock(&q->mutex);
            return false;
        }
        q->tasks = new_arr;
        q->capacity = new_cap;
    }
    q->tasks[q->count++] = path;
    pthread_mutex_unlock(&q->mutex);
    return true;
}

void lost_tasks_push_backlog(LostTasksQueue *q, char **backlog_paths, int backlog_count) {
    if (!q || !backlog_paths || backlog_count <= 0) return;
    pthread_mutex_lock(&q->mutex);
    for (int j = 0; j < backlog_count; j++) {
        char *path = backlog_paths[j];
        if (!path) continue;
        if (q->count >= q->capacity) {
            size_t new_cap = q->capacity ? q->capacity * 2 : 64;
            char **new_arr = realloc(q->tasks, new_cap * sizeof(char *));
            if (!new_arr) {
                log_warn("LostTasksQueue realloc failed, dropping %s", path);
                free(path);
                continue;
            }
            q->tasks = new_arr;
            q->capacity = new_cap;
        }
        q->tasks[q->count++] = path;
        backlog_paths[j] = NULL;
    }
    pthread_mutex_unlock(&q->mutex);
}

bool lost_tasks_pop(LostTasksQueue *q, char **path_out) {
    if (!q || !path_out) return false;
    pthread_mutex_lock(&q->mutex);
    if (q->count == 0) {
        pthread_mutex_unlock(&q->mutex);
        return false;
    }
    /* 找到第一个非 NULL 的任务 */
    size_t idx = 0;
    while (idx < q->count && q->tasks[idx] == NULL) {
        idx++;
    }
    if (idx >= q->count) {
        q->count = 0;
        pthread_mutex_unlock(&q->mutex);
        return false;
    }
    *path_out = q->tasks[idx];
    q->tasks[idx] = NULL;
    pthread_mutex_unlock(&q->mutex);
    return true;
}

void lost_tasks_compact(LostTasksQueue *q) {
    if (!q) return;
    pthread_mutex_lock(&q->mutex);
    if (q->count == 0) {
        pthread_mutex_unlock(&q->mutex);
        return;
    }
    size_t new_count = 0;
    for (size_t i = 0; i < q->count; i++) {
        if (q->tasks[i]) {
            q->tasks[new_count++] = q->tasks[i];
        }
    }
    q->count = new_count;
    pthread_mutex_unlock(&q->mutex);
}

size_t lost_tasks_count(LostTasksQueue *q) {
    if (!q) return 0;
    pthread_mutex_lock(&q->mutex);
    size_t n = q->count;
    pthread_mutex_unlock(&q->mutex);
    return n;
}

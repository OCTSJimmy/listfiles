#include "probe_scheduler.h"
#include "spbin.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ================================================================
 * Min-heap ordered by next_probe_time
 * ================================================================ */

static inline void swap_task(ProbeTask *a, ProbeTask *b) {
    ProbeTask t = *a; *a = *b; *b = t;
}

static void heap_up(ProbeScheduler *sched, size_t idx) {
    while (idx > 0) {
        size_t parent = (idx - 1) >> 1;
        if (sched->tasks[parent].next_probe_time <= sched->tasks[idx].next_probe_time)
            break;
        swap_task(&sched->tasks[parent], &sched->tasks[idx]);
        idx = parent;
    }
}

static void heap_down(ProbeScheduler *sched, size_t idx) {
    size_t n = sched->count;
    while (1) {
        size_t left = (idx << 1) + 1;
        size_t right = left + 1;
        size_t smallest = idx;

        if (left < n && sched->tasks[left].next_probe_time < sched->tasks[smallest].next_probe_time)
            smallest = left;
        if (right < n && sched->tasks[right].next_probe_time < sched->tasks[smallest].next_probe_time)
            smallest = right;

        if (smallest == idx) break;
        swap_task(&sched->tasks[idx], &sched->tasks[smallest]);
        idx = smallest;
    }
}

ProbeScheduler* probe_scheduler_create(void) {
    ProbeScheduler *sched = calloc(1, sizeof(ProbeScheduler));
    if (!sched) return NULL;
    sched->capacity = 64;
    sched->tasks = calloc(sched->capacity, sizeof(ProbeTask));
    if (!sched->tasks) { free(sched); return NULL; }
    sched->count = 0;
    pthread_mutex_init(&sched->mutex, NULL);
    return sched;
}

void probe_scheduler_destroy(ProbeScheduler *sched) {
    if (!sched) return;
    pthread_mutex_destroy(&sched->mutex);
    free(sched->tasks);
    free(sched);
}

void probe_scheduler_push(ProbeScheduler *sched, const ProbeTask *task) {
    pthread_mutex_lock(&sched->mutex);
    if (sched->count >= sched->capacity) {
        size_t new_cap = sched->capacity << 1;
        ProbeTask *new_tasks = realloc(sched->tasks, new_cap * sizeof(ProbeTask));
        if (!new_tasks) { pthread_mutex_unlock(&sched->mutex); return; }
        sched->tasks = new_tasks;
        sched->capacity = new_cap;
    }
    sched->tasks[sched->count] = *task;
    heap_up(sched, sched->count);
    sched->count++;
    pthread_mutex_unlock(&sched->mutex);
}

bool probe_scheduler_peek(const ProbeScheduler *sched, ProbeTask *out) {
    pthread_mutex_lock((pthread_mutex_t*)&sched->mutex);
    if (sched->count == 0) { pthread_mutex_unlock((pthread_mutex_t*)&sched->mutex); return false; }
    time_t now = time(NULL);
    if (sched->tasks[0].next_probe_time > now) { pthread_mutex_unlock((pthread_mutex_t*)&sched->mutex); return false; }
    if (out) *out = sched->tasks[0];
    pthread_mutex_unlock((pthread_mutex_t*)&sched->mutex);
    return true;
}

/* Pop the top element (caller should have peeked first) */
static void heap_pop(ProbeScheduler *sched) {
    if (sched->count == 0) return;
    sched->count--;
    if (sched->count > 0) {
        sched->tasks[0] = sched->tasks[sched->count];
        heap_down(sched, 0);
    }
}

void probe_scheduler_remove_dev(ProbeScheduler *sched, dev_t dev) {
    pthread_mutex_lock(&sched->mutex);
    for (size_t i = 0; i < sched->count; i++) {
        if (sched->tasks[i].dev == dev) {
            sched->count--;
            if (i < sched->count) {
                sched->tasks[i] = sched->tasks[sched->count];
                heap_up(sched, i);
                heap_down(sched, i);
            }
            pthread_mutex_unlock(&sched->mutex);
            return;
        }
    }
    pthread_mutex_unlock(&sched->mutex);
}

void probe_scheduler_mark_condemned(ProbeScheduler *sched, dev_t dev) {
    pthread_mutex_lock(&sched->mutex);
    for (size_t i = 0; i < sched->count; i++) {
        if (sched->tasks[i].dev == dev) {
            sched->tasks[i].s_status = SP_STATUS_CONDEMNED;
            pthread_mutex_unlock(&sched->mutex);
            return;
        }
    }
    pthread_mutex_unlock(&sched->mutex);
}

/* After a probe failure: pop the due task, update interval, and push back */
void probe_scheduler_reschedule_after_failure(ProbeScheduler *sched) {
    pthread_mutex_lock(&sched->mutex);
    if (sched->count == 0) { pthread_mutex_unlock(&sched->mutex); return; }
    time_t now = time(NULL);
    if (sched->tasks[0].next_probe_time > now) { pthread_mutex_unlock(&sched->mutex); return; }

    ProbeTask task = sched->tasks[0];
    heap_pop(sched);

    task.retry_count++;
    task.probe_interval *= 2;
    if (task.probe_interval > PROBE_INTERVAL_MAX)
        task.probe_interval = PROBE_INTERVAL_MAX;
    task.next_probe_time = now + task.probe_interval;

    sched->tasks[sched->count] = task;
    heap_up(sched, sched->count);
    sched->count++;
    pthread_mutex_unlock(&sched->mutex);
}

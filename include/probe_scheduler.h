#ifndef PROBE_SCHEDULER_H
#define PROBE_SCHEDULER_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <sys/types.h>
#include <pthread.h>

#define PROBE_TIMEOUT_SEC       5
#define PROBE_INTERVAL_INITIAL  5
#define PROBE_INTERVAL_MAX      300

typedef struct {
    dev_t    dev;
    char     probe_path[4096];
    time_t   next_probe_time;
    uint32_t probe_interval;
    uint32_t retry_count;
    uint8_t  s_status;         /* SP_STATUS_PROBING or CONDEMNED */
} ProbeTask;

typedef struct {
    ProbeTask *tasks;          /* min-heap ordered by next_probe_time */
    size_t     count;
    size_t     capacity;
    pthread_mutex_t mutex;     /* thread-safe wrapper */
} ProbeScheduler;

ProbeScheduler* probe_scheduler_create(void);
void probe_scheduler_destroy(ProbeScheduler *sched);
void probe_scheduler_push(ProbeScheduler *sched, const ProbeTask *task);
bool probe_scheduler_peek(const ProbeScheduler *sched, ProbeTask *out); /* true if a task is due (copied, not removed) */
void probe_scheduler_remove_dev(ProbeScheduler *sched, dev_t dev);
void probe_scheduler_mark_condemned(ProbeScheduler *sched, dev_t dev);

#endif

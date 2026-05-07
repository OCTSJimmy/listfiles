#ifndef MONITOR_H
#define MONITOR_H

#include <pthread.h>
#include <stdbool.h>
#include <sys/types.h>

struct AppContext;

typedef struct Monitor {
    struct AppContext *ctx;
    bool running;
    pthread_t tid;
    pid_t active_probe_pid;
    dev_t active_probe_dev;
} Monitor;

Monitor* monitor_create(struct AppContext *ctx);
void monitor_destroy(Monitor *mon);
void* monitor_thread_entry(void *arg);

#endif

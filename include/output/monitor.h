#ifndef MONITOR_H
#define MONITOR_H

#include <pthread.h>
#include <stdbool.h>
#include <sys/types.h>
#include <stdint.h>

struct AppContext;

typedef struct Monitor {
    struct AppContext *ctx;
    bool running;
    pthread_t tid;
    pid_t active_probe_pid;
    dev_t active_probe_dev;
    uint32_t active_probe_retry_count;
    uint32_t active_probe_interval;
    /* v15.6.2：探测路径留档——子进程退出码携带 lstat errno，父进程据此区分
     * "设备死"与"设备活但路径坏"（非法文件名不得重入队，改判 INVALID_NAME） */
    char active_probe_path[4096];
    /* v15.6.1（P0-105）：有效进展看门狗——file+dir 计数无增长且仍有应做工作
     * 持续超 --stall-timeout 秒 → 输出现场并以非零码退出（或 abort） */
    unsigned long wd_last_total;
    time_t        wd_last_change;
} Monitor;

Monitor* monitor_create(struct AppContext *ctx);
void monitor_destroy(Monitor *mon);
void* monitor_thread_entry(void *arg);

#endif

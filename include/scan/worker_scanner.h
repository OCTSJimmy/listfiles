#ifndef WORKER_SCANNER_H
#define WORKER_SCANNER_H

#include <stdbool.h>
#include <time.h>
#include <pthread.h>
#include "config.h"
#include "fingerprint_set.h"
#include "reference_map.h"

/* Worker 内部多线程上下文 (v14.0.0) */
typedef struct {
    int fd_cmd;
    int fd_data;
    int fd_ctrl;
    int worker_id;

    /* 任务同步 */
    pthread_mutex_t task_mutex;
    pthread_cond_t  task_cond;
    char   task_path[4096];
    bool   task_ready;
    bool   stop_flag;

    /* Scanner 进度监控 */
    pthread_mutex_t progress_mutex;
    time_t last_progress;
    bool   scanner_active;
} WorkerThreadCtx;

/* 设置 Worker 只读上下文（fork 前由主进程调用） */
void worker_set_context(const Config *cfg, const FingerprintSet *ref_set, const ReferenceMap *ref_map);

/* 获取当前 Worker 配置指针（供 IPC 线程查询 heartbeat_timeout 等） */
const Config* worker_get_config(void);

/* Scanner 线程入口 */
void *worker_scanner_thread(void *arg);

#endif

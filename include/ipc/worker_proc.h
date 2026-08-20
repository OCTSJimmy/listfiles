#ifndef WORKER_PROC_H
#define WORKER_PROC_H

#include <stdatomic.h>
#include <stdbool.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include "config.h"
#include "fingerprint_set.h"
#include "reference_map.h"
#include "ipc_protocol.h"
#include "worker_scanner.h"

/* Worker 显式状态机 (v15.1.0) */
#define WORKER_STATE_IDLE         0
#define WORKER_STATE_BUSY         1
#define WORKER_STATE_DEAD         2
#define WORKER_STATE_INITIALIZING 3

/* v15.6.0: 在途目录任务状态（P0-001 完成屏障，仅主线程写） */
enum {
    DT_NONE = 0,            /* 无在途任务 */
    DT_SCANNING,            /* 已派发，Worker 扫描中 */
    DT_BATCHES_RECEIVED,    /* 收到 FINISH，本任务全部 BATCH 已到齐 */
    DT_BATCHES_PROCESSED,   /* 线程池已处理完本任务全部 BATCH */
    DT_COMPLETED            /* 输出 COMMITTED，dpbin 已写，任务完结 */
};

typedef struct {
    int      slot_id;
    pid_t    pid;
    int      fd_cmd;           /* master write end (M→W commands) */
    int      fd_cmd_rd;        /* master read end of fd_cmd pipe (draining) */
    int      fd_data;          /* master read end (W→M BATCH data) */
    int      fd_ctrl;          /* master read end (W→M control signals) */
    _Atomic time_t last_heartbeat;
    _Atomic bool   is_alive;
    _Atomic int    state;      /* WORKER_STATE_IDLE / BUSY / DEAD (v15.1.0) */
    uint64_t current_dev;
    uint64_t current_epoch;  /* v15.6.0: 当前在途任务 epoch，Master 校验 RET_BATCH/RET_FINISH */
    char     current_path[4096]; /* 从 40 扩展到 4096，防止路径截断 */
    /* v15.6.0: 在途目录任务屏障（P0-001/P0-002） */
    struct stat current_st;          /* 当前任务目录的 stat（dispatch 时保存，供 dpbin） */
    _Atomic long batches_received;   /* Master 已收到的本任务 BATCH 数 */
    _Atomic long batches_processed;  /* 线程池已处理完的本任务 BATCH 数 */
    int          task_state;         /* DT_* 任务状态（仅主线程写） */
    char   **backlog_paths;
    int      backlog_count;
    int      backlog_capacity;
    atomic_flag cleanup_done;   /* 防止 monitor 和 epoll 并发 cleanup 的竞态 */
} WorkerSlot;

typedef struct {
    WorkerSlot *slots;
    int         num_workers;
    _Atomic int active_count;
} WorkerPool;

/* Master-side */
WorkerPool* worker_pool_create(int num_workers);
void        worker_pool_destroy(WorkerPool *pool);
bool        worker_pool_spawn(WorkerPool *pool, int slot_id);
bool        worker_pool_replace(WorkerPool *pool, int slot_id);
void        worker_pool_stop_all(WorkerPool *pool);

/* Master-side */
WorkerPool* worker_pool_create(int num_workers);
void        worker_pool_destroy(WorkerPool *pool);
bool        worker_pool_spawn(WorkerPool *pool, int slot_id);
bool        worker_pool_replace(WorkerPool *pool, int slot_id);
void        worker_pool_stop_all(WorkerPool *pool);

/* Worker-side */
void worker_main(int fd_cmd, int fd_data, int fd_ctrl, int worker_id);

/* Forward declaration to break circular dependency with app_context.h */
struct AppContext;

/* Main loop cleanup */
void cleanup_dead_worker_slot(struct AppContext *ctx, int worker_id, bool redispatch_current);

#endif

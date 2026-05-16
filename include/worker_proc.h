#ifndef WORKER_PROC_H
#define WORKER_PROC_H

#include <stdatomic.h>
#include <stdbool.h>
#include <time.h>
#include <sys/types.h>
#include "config.h"
#include "fingerprint_set.h"
#include "reference_map.h"
#include "ipc_protocol.h"

/* Worker 显式状态机 (v15.1.0) */
#define WORKER_STATE_IDLE  0
#define WORKER_STATE_BUSY  1
#define WORKER_STATE_DEAD  2

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
    char     current_path[4096]; /* 从 40 扩展到 4096，防止路径截断 */
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

/* IPC */
int ipc_send(int fd, uint32_t msg_type, const void *payload, uint32_t payload_len);
int ipc_recv_header(int fd, IpcMessageHeader *hdr);
int ipc_recv_payload(int fd, void *buf, uint32_t len);
int ipc_drain_and_count_tasks(int fd_in);

/* Worker-side */
void worker_set_context(const Config *cfg, const FingerprintSet *ref_set, const ReferenceMap *ref_map);
void worker_main(int fd_cmd, int fd_data, int fd_ctrl, int worker_id);

/* Forward declaration to break circular dependency with app_context.h */
struct AppContext;

/* Main loop cleanup */
void cleanup_dead_worker_slot(struct AppContext *ctx, int worker_id, bool redispatch_current);

#endif

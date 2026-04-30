#ifndef WORKER_PROC_H
#define WORKER_PROC_H

#include <stdbool.h>
#include <time.h>
#include <sys/types.h>
#include "config.h"
#include "fingerprint_set.h"
#include "reference_map.h"
#include "ipc_protocol.h"

typedef struct {
    int      slot_id;
    pid_t    pid;
    int      fd_in;            /* master write end */
    int      fd_out;           /* master read end */
    time_t   last_heartbeat;
    bool     is_alive;
    uint64_t current_dev;
    char     current_path[4096];
} WorkerSlot;

typedef struct {
    WorkerSlot *slots;
    int         num_workers;
    int         active_count;
} WorkerPool;

/* Master-side API */
WorkerPool* worker_pool_create(int num_workers);
void worker_pool_destroy(WorkerPool *pool);
bool worker_pool_spawn(WorkerPool *pool, int slot_id);
bool worker_pool_replace(WorkerPool *pool, int slot_id);
void worker_pool_stop_all(WorkerPool *pool);

/* Set read-only context before spawning workers (fork-safe via COW) */
void worker_set_context(const Config *cfg, const FingerprintSet *ref_set, const ReferenceMap *ref_map);

/* IPC helpers (used by both master and worker) */
int ipc_send(int fd, uint32_t msg_type, const void *payload, uint32_t payload_len);
int ipc_recv_header(int fd, IpcMessageHeader *hdr);
int ipc_recv_payload(int fd, void *buf, uint32_t len);

/* Worker process entry point (called in child after fork) */
void worker_main(int fd_in, int fd_out, int worker_id);

#endif

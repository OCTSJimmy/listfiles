#ifndef MAIN_LOOP_H
#define MAIN_LOOP_H

#include "app_context.h"

void main_loop_run(AppContext *ctx);

/* IPC thread lifecycle (v13.0.0) */
bool init_ipc_threads(AppContext *ctx);
void destroy_ipc_threads(AppContext *ctx);
void stop_all_ipc_threads(AppContext *ctx);

/* IPC helper: send REPLACE to IPC thread (used by main.c for initial bootstrap) */
void send_replace_to_ipc(AppContext *ctx, int wid, int fd_in, int fd_out, pid_t pid);

/* Worker death cleanup: drain fd_in_rd, migrate backlog, adjust pending_tasks, close fds */
void cleanup_dead_worker_slot(AppContext *ctx, int worker_id, bool redispatch_current);

/* Internal handlers exposed for testing if needed */
void main_loop_handle_batch(AppContext *ctx, int worker_id, const void *payload, uint32_t len);
void main_loop_handle_heartbeat(AppContext *ctx, int worker_id, uint64_t timestamp);
void main_loop_handle_error(AppContext *ctx, int worker_id, const IpcErrorHeader *err, const char *path);
void main_loop_handle_exit(AppContext *ctx, int worker_id);

#endif

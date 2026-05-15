#ifndef IPC_THREAD_H
#define IPC_THREAD_H

#include "msg_queue.h"
#include "worker_proc.h"

/* ================================================================
 * IPC Thread (v13.0.0)
 * One thread per Worker, manages non-blocking epoll + heartbeat.
 * Communicates with Master Thread via lock-free message queues.
 * ================================================================ */

typedef struct {
    int             slot_id;        /* Worker slot index */
    WorkerPool     *pool;           /* Reference to WorkerPool */
    MsgQueue       *cmd_queue;      /* Commands from Master Thread */
    MsgQueue       *ret_queue;      /* Returns to Master Thread */
    int             epfd;           /* IPC thread's own epoll */
    _Atomic bool    running;
    _Atomic time_t  last_heartbeat;
    int             fd_in;          /* Current Worker write end */
    int             fd_out;         /* Current Worker read end */
    pid_t           pid;            /* Current Worker pid */
    _Atomic bool    waiting_replace;/* Set after DEAD, cleared after REPLACE */
    int             eagain_retry_count; /* EAGAIN retry counter (reset on REPLACE) */
    pthread_cond_t *master_cond;    /* Signal master thread when message sent */
} IpcThreadCtx;

/**
 * @brief  Create IPC thread context
 * @param  slot_id  int       Worker slot index
 * @param  pool     WorkerPool*  reference to pool (for slot state)
 * @param  cmd      MsgQueue*    command queue from master
 * @param  ret      MsgQueue*    return queue to master
 * @param  master_cond pthread_cond_t*  master thread condition variable to signal
 * @return IpcThreadCtx* or NULL
 */
IpcThreadCtx* ipc_thread_ctx_create(int slot_id, WorkerPool *pool,
                                   MsgQueue *cmd, MsgQueue *ret,
                                   pthread_cond_t *master_cond);

/**
 * @brief  Destroy IPC thread context (closes fds, stops thread)
 */
void ipc_thread_ctx_destroy(IpcThreadCtx *ctx);

/**
 * @brief  Main IPC thread loop (pthread-compatible)
 * @param  arg  void*  IpcThreadCtx*
 * @return void*
 */
void* ipc_thread_loop(void *arg);

/**
 * @brief  Signal IPC thread to stop
 */
void ipc_thread_stop(IpcThreadCtx *ctx);

#endif

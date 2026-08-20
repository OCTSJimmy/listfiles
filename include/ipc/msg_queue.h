#ifndef MSG_QUEUE_H
#define MSG_QUEUE_H

#include "msg_format.h"
#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>

/* ================================================================
 * Mutex-protected ring buffer message queue (v13.0.0)
 * Capacity must be power of 2. head/tail guarded by mutex, eventfd notify.
 * ================================================================ */

/* v15.6.0: 1024 -> 65536，使容量瓶颈永远在 dispatch_queue 而非 IPC 层。
 * 满即设计外异常（log_fatal），正常路径禁止丢消息。 */
#define MSG_QUEUE_DEFAULT_CAPACITY 65536

typedef struct {
    IpcThreadMsg  *buffer;      /* ring buffer storage */
    size_t         capacity;    /* must be power of 2 */
    size_t         head;        /* consumer read index */
    size_t         tail;        /* producer write index */
    int            eventfd;     /* notification fd (EFD_SEMAPHORE) */
    pthread_mutex_t mutex;      /* protects head/tail and buffer access */
} MsgQueue;

/**
 * @brief  Create a new message queue
 * @param  cap  size_t  capacity, must be power of 2
 * @return MsgQueue* or NULL on failure
 */
MsgQueue* msg_queue_create(size_t cap);

/**
 * @brief  Destroy queue and free all undelivered messages
 */
void msg_queue_destroy(MsgQueue *q);

/**
 * @brief  Send a message (non-blocking, mutex-protected)
 * @return true if queued, false if full (caller should retry or backpressure)
 */
bool msg_queue_send(MsgQueue *q, const IpcThreadMsg *msg);

/**
 * @brief  Receive a message (non-blocking, mutex-protected)
 * @return true if a message was popped, false if empty
 */
bool msg_queue_recv(MsgQueue *q, IpcThreadMsg *out);

/**
 * @brief  Block until a message arrives or timeout
 * @param  timeout_ms  int  -1 = block forever, 0 = non-blocking, >0 = timeout ms
 * @return true if received, false if timeout/empty
 */
bool msg_queue_recv_wait(MsgQueue *q, IpcThreadMsg *out, int timeout_ms);

/**
 * @brief  Drain eventfd counter (call after waking up)
 */
void msg_queue_drain_eventfd(MsgQueue *q);

#endif

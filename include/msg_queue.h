#ifndef MSG_QUEUE_H
#define MSG_QUEUE_H

#include "msg_format.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

/* ================================================================
 * Lock-free ring buffer message queue (v13.0.0)
 * Capacity must be power of 2. Uses 64-bit atomic head/tail + eventfd notify.
 * ================================================================ */

#define MSG_QUEUE_DEFAULT_CAPACITY 1024

typedef struct {
    IpcThreadMsg  *buffer;      /* ring buffer storage */
    size_t         capacity;    /* must be power of 2 */
    _Atomic size_t head;        /* consumer read index */
    _Atomic size_t tail;        /* producer write index */
    int            eventfd;     /* notification fd (EFD_SEMAPHORE) */
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
 * @brief  Send a message (non-blocking, lock-free)
 * @return true if queued, false if full (caller should retry or backpressure)
 */
bool msg_queue_send(MsgQueue *q, const IpcThreadMsg *msg);

/**
 * @brief  Receive a message (non-blocking, lock-free)
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

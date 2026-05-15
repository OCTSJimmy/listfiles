#define _GNU_SOURCE
#include "msg_queue.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/eventfd.h>

/* ================================================================
 * Lock-free ring buffer using 64-bit atomic head/tail
 * Capacity must be power of 2 for mask-based indexing.
 * ================================================================ */

MsgQueue* msg_queue_create(size_t cap) {
    if (cap == 0 || (cap & (cap - 1)) != 0) {
        log_error("msg_queue_create: capacity %zu is not power of 2", cap);
        return NULL;
    }
    MsgQueue *q = calloc(1, sizeof(MsgQueue));
    if (!q) return NULL;

    q->buffer = calloc(cap, sizeof(IpcThreadMsg));
    if (!q->buffer) {
        free(q);
        return NULL;
    }

    q->capacity = cap;
    atomic_init(&q->head, 0);
    atomic_init(&q->tail, 0);

    q->eventfd = eventfd(0, EFD_SEMAPHORE | EFD_NONBLOCK | EFD_CLOEXEC);
    if (q->eventfd < 0) {
        log_error("msg_queue_create: eventfd failed: %s", strerror(errno));
        free(q->buffer);
        free(q);
        return NULL;
    }

    return q;
}

void msg_queue_destroy(MsgQueue *q) {
    if (!q) return;

    /* Drain and free all undelivered messages */
    IpcThreadMsg msg;
    while (msg_queue_recv(q, &msg)) {
        free(msg.data);
    }

    if (q->eventfd >= 0) close(q->eventfd);
    free(q->buffer);
    free(q);
}

bool msg_queue_send(MsgQueue *q, const IpcThreadMsg *msg) {
    if (!q || !msg) return false;

    size_t cap = q->capacity;
    size_t mask = cap - 1;

    while (1) {
        size_t tail = atomic_load_explicit(&q->tail, memory_order_relaxed);
        size_t head = atomic_load_explicit(&q->head, memory_order_acquire);

        if (tail - head >= cap) {
            /* Queue full */
            return false;
        }

        /* Try to reserve slot */
        if (atomic_compare_exchange_weak_explicit(
                &q->tail, &tail, tail + 1,
                memory_order_relaxed, memory_order_relaxed)) {
            /* Write data */
            q->buffer[tail & mask] = *msg;
            /* Publish write */
            atomic_thread_fence(memory_order_release);

            /* Notify consumer via eventfd */
            uint64_t inc = 1;
            (void)write(q->eventfd, &inc, sizeof(inc));
            return true;
        }
        /* CAS failed, retry */
    }
}

bool msg_queue_recv(MsgQueue *q, IpcThreadMsg *out) {
    if (!q || !out) return false;

    size_t cap = q->capacity;
    size_t mask = cap - 1;

    while (1) {
        size_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
        size_t tail = atomic_load_explicit(&q->tail, memory_order_acquire);

        if (head >= tail) {
            /* Queue empty */
            return false;
        }

        /* Try to reserve read */
        if (atomic_compare_exchange_weak_explicit(
                &q->head, &head, head + 1,
                memory_order_relaxed, memory_order_relaxed)) {
            /* Acquire fence to ensure we see the write */
            atomic_thread_fence(memory_order_acquire);
            *out = q->buffer[head & mask];
            return true;
        }
        /* CAS failed, retry */
    }
}

bool msg_queue_recv_wait(MsgQueue *q, IpcThreadMsg *out, int timeout_ms) {
    if (!q || !out) return false;

    /* Fast path: try non-blocking first */
    if (msg_queue_recv(q, out)) return true;

    /* Slow path: block on eventfd */
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(q->eventfd, &fds);

    struct timeval tv, *tvp = NULL;
    if (timeout_ms >= 0) {
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        tvp = &tv;
    }

    int rc = select(q->eventfd + 1, &fds, NULL, NULL, tvp);
    if (rc > 0) {
        /* Drain eventfd */
        uint64_t val;
        (void)read(q->eventfd, &val, sizeof(val));
        return msg_queue_recv(q, out);
    }
    return false;
}

void msg_queue_drain_eventfd(MsgQueue *q) {
    if (!q || q->eventfd < 0) return;
    uint64_t val;
    while (read(q->eventfd, &val, sizeof(val)) > 0) {
        /* drain all notifications */
    }
}

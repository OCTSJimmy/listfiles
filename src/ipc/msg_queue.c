#define _GNU_SOURCE
#include "msg_queue.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/eventfd.h>
#include <pthread.h>

/* ================================================================
 * Simple mutex-protected ring buffer message queue
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
    q->head = 0;
    q->tail = 0;

    if (pthread_mutex_init(&q->mutex, NULL) != 0) {
        log_error("msg_queue_create: mutex init failed");
        free(q->buffer);
        free(q);
        return NULL;
    }

    q->eventfd = eventfd(0, EFD_SEMAPHORE | EFD_NONBLOCK | EFD_CLOEXEC);
    if (q->eventfd < 0) {
        log_error("msg_queue_create: eventfd failed: %s", strerror(errno));
        pthread_mutex_destroy(&q->mutex);
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
    pthread_mutex_destroy(&q->mutex);
    free(q->buffer);
    free(q);
}

bool msg_queue_send(MsgQueue *q, const IpcThreadMsg *msg) {
    if (!q || !msg) return false;

    size_t cap = q->capacity;
    size_t mask = cap - 1;

    pthread_mutex_lock(&q->mutex);

    size_t tail = q->tail;
    size_t head = q->head;

    if (tail - head >= cap) {
        /* Queue full */
        pthread_mutex_unlock(&q->mutex);
        return false;
    }

    /* Write data */
    q->buffer[tail & mask] = *msg;
    q->tail = tail + 1;

    log_debug("[Queue] SEND head=%zu tail=%zu type=%u slot=%d", head, q->tail, msg->type, msg->slot_id);

    pthread_mutex_unlock(&q->mutex);

    /* Notify consumer via eventfd */
    uint64_t inc = 1;
    (void)write(q->eventfd, &inc, sizeof(inc));
    return true;
}

bool msg_queue_recv(MsgQueue *q, IpcThreadMsg *out) {
    if (!q || !out) return false;

    size_t cap = q->capacity;
    size_t mask = cap - 1;

    pthread_mutex_lock(&q->mutex);

    size_t head = q->head;
    size_t tail = q->tail;

    if (head >= tail) {
        /* Queue empty */
        pthread_mutex_unlock(&q->mutex);
        return false;
    }

    *out = q->buffer[head & mask];
    q->head = head + 1;

    log_debug("[Queue] RECV head=%zu tail=%zu type=%u slot=%d", q->head, tail, out->type, out->slot_id);

    pthread_mutex_unlock(&q->mutex);
    return true;
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

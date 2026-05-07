#include "thread_pool.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/eventfd.h>
#include <stdio.h>

#define TP_QUEUE_CAPACITY 256

typedef struct CompletedNode {
    TPBatch *batch;
    struct CompletedNode *next;
} CompletedNode;

struct ThreadPool {
    int num_threads;
    pthread_t *threads;
    
    /* 工作队列（环形数组） */
    TPBatch *queue[TP_QUEUE_CAPACITY];
    int head, tail, count;
    pthread_mutex_t queue_mutex;
    pthread_cond_t queue_cond;
    
    /* 完成队列（单链表） */
    CompletedNode *completed_head;
    CompletedNode *completed_tail;
    pthread_mutex_t completed_mutex;
    
    int event_fd;
    bool stop;
    
    tp_process_fn process_fn;
    void *user_data;
};

static void *worker_thread(void *arg) {
    ThreadPool *tp = arg;
    while (1) {
        pthread_mutex_lock(&tp->queue_mutex);
        while (!tp->stop && tp->count == 0) {
            pthread_cond_wait(&tp->queue_cond, &tp->queue_mutex);
        }
        if (tp->stop && tp->count == 0) {
            pthread_mutex_unlock(&tp->queue_mutex);
            break;
        }
        TPBatch *batch = tp->queue[tp->head];
        tp->head = (tp->head + 1) % TP_QUEUE_CAPACITY;
        tp->count--;
        pthread_mutex_unlock(&tp->queue_mutex);
        
        /* 执行 CPU 密集的去重计算 */
        tp->process_fn(batch, tp->user_data);
        
        /* 加入完成队列 */
        CompletedNode *node = malloc(sizeof(CompletedNode));
        if (!node) {
            fprintf(stderr, "[Fatal] thread_pool: out of memory for completed node\n");
            /* 继续，但会丢失这个 batch 的通知 */
        } else {
            node->batch = batch;
            node->next = NULL;
            pthread_mutex_lock(&tp->completed_mutex);
            if (tp->completed_tail) {
                tp->completed_tail->next = node;
            } else {
                tp->completed_head = node;
            }
            tp->completed_tail = node;
            pthread_mutex_unlock(&tp->completed_mutex);
            
            /* 通知主线程 */
            eventfd_write(tp->event_fd, 1);
        }
    }
    return NULL;
}

ThreadPool* thread_pool_create(int num_threads, int event_fd, tp_process_fn fn, void *user_data) {
    if (num_threads < 1) num_threads = 1;
    ThreadPool *tp = calloc(1, sizeof(ThreadPool));
    if (!tp) return NULL;
    
    tp->num_threads = num_threads;
    tp->event_fd = event_fd;
    tp->process_fn = fn;
    tp->user_data = user_data;
    tp->stop = false;
    
    pthread_mutex_init(&tp->queue_mutex, NULL);
    pthread_cond_init(&tp->queue_cond, NULL);
    pthread_mutex_init(&tp->completed_mutex, NULL);
    
    tp->threads = calloc(num_threads, sizeof(pthread_t));
    if (!tp->threads) {
        free(tp);
        return NULL;
    }
    
    for (int i = 0; i < num_threads; i++) {
        if (pthread_create(&tp->threads[i], NULL, worker_thread, tp) != 0) {
            tp->stop = true;
            pthread_cond_broadcast(&tp->queue_cond);
            for (int j = 0; j < i; j++) {
                pthread_join(tp->threads[j], NULL);
            }
            free(tp->threads);
            pthread_mutex_destroy(&tp->queue_mutex);
            pthread_cond_destroy(&tp->queue_cond);
            pthread_mutex_destroy(&tp->completed_mutex);
            free(tp);
            return NULL;
        }
    }
    return tp;
}

void thread_pool_destroy(ThreadPool *tp) {
    if (!tp) return;
    
    pthread_mutex_lock(&tp->queue_mutex);
    tp->stop = true;
    pthread_cond_broadcast(&tp->queue_cond);
    pthread_mutex_unlock(&tp->queue_mutex);
    
    for (int i = 0; i < tp->num_threads; i++) {
        pthread_join(tp->threads[i], NULL);
    }
    free(tp->threads);
    
    /* 清理完成队列中残留的 batch（不执行副作用，直接释放内存） */
    TPBatch *batch;
    while ((batch = thread_pool_poll_completed(tp)) != NULL) {
        for (int i = 0; i < batch->count; i++) free(batch->paths[i]);
        free(batch->paths);
        free(batch->stats);
        free(batch->results);
        free(batch);
    }
    
    pthread_mutex_destroy(&tp->queue_mutex);
    pthread_cond_destroy(&tp->queue_cond);
    pthread_mutex_destroy(&tp->completed_mutex);
    free(tp);
}

bool thread_pool_submit(ThreadPool *tp, TPBatch *batch) {
    if (!tp || !batch) return false;
    pthread_mutex_lock(&tp->queue_mutex);
    if (tp->count >= TP_QUEUE_CAPACITY) {
        pthread_mutex_unlock(&tp->queue_mutex);
        return false;
    }
    tp->queue[tp->tail] = batch;
    tp->tail = (tp->tail + 1) % TP_QUEUE_CAPACITY;
    tp->count++;
    pthread_cond_signal(&tp->queue_cond);
    pthread_mutex_unlock(&tp->queue_mutex);
    return true;
}

TPBatch* thread_pool_poll_completed(ThreadPool *tp) {
    if (!tp) return NULL;
    pthread_mutex_lock(&tp->completed_mutex);
    CompletedNode *node = tp->completed_head;
    if (node) {
        tp->completed_head = node->next;
        if (!tp->completed_head) {
            tp->completed_tail = NULL;
        }
    }
    pthread_mutex_unlock(&tp->completed_mutex);
    
    if (!node) return NULL;
    TPBatch *batch = node->batch;
    free(node);
    return batch;
}

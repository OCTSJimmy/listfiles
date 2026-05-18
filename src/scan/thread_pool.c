/**
 * @file thread_pool.c
 * @brief Master 内嵌 CPU 去重线程池实现
 *
 * 采用 mutex + cond + 有界环形队列的工作线程模型，配合 eventfd 通知主线程。
 * 将 CPU 密集型的指纹计算与设备黑名单检查 offload 到工作线程，
 * 避免阻塞 epoll 主循环。
 * 队列满时自动降级为同步处理（由调用方在主线程直接执行）。
 */
#include "thread_pool.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/eventfd.h>
#include <stdio.h>
#include "log.h"

#define TP_QUEUE_CAPACITY 256

/**
 * @brief 线程池完成队列链表节点
 *
 * 工作线程处理完 batch 后，将结果封装为 CompletedNode 挂入完成队列，
 * 并通过 eventfd 写入通知主线程 epoll 唤醒。
 */
typedef struct CompletedNode {
    TPBatch *batch;
    struct CompletedNode *next;
} CompletedNode;

/**
 * @brief 线程池内部结构体（ opaque 类型，对外部隐藏实现细节）
 *
 * 包含工作队列（环形数组）、完成队列（单链表）、eventfd、停止标志及线程数组。
 * 所有队列操作均由独立 mutex 保护。
 */
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

/**
 * @brief  工作线程主函数
 * @param  arg  void*  指向 ThreadPool 结构体的指针，不能为空
 * @return void*  始终返回 NULL
 *
 * @note   线程启动后进入循环：等待 queue_cond（队列非空或 stop 标志）→
 *         从环形队列取出 TPBatch → 调用 process_fn 执行去重计算 →
 *         将 batch 挂入完成队列 → 通过 eventfd_write 通知主线程。
 *         若 malloc CompletedNode 失败，则丢失该 batch 的通知（但 batch 仍会被处理）。
 */
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
            log_fatal("thread_pool: out of memory for completed node");
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

/**
 * @brief  创建线程池实例
 * @param  num_threads  int              工作线程数量，取值范围: >= 1（传入 <1 时自动修正为 1）
 * @param  event_fd     int              用于通知主线程的 eventfd 文件描述符，取值范围: >= 0 的有效 fd
 * @param  fn           tp_process_fn    工作线程回调函数指针，不能为空
 * @param  user_data    void*            回调函数的用户数据指针，允许为 NULL
 * @return ThreadPool*  成功返回指向新分配线程池的指针；创建失败时返回 NULL
 *
 * @note   创建失败时会自动清理已创建的线程和已分配的内存，不会泄漏资源。
 *         所有线程创建成功后才返回；若有任何线程创建失败，则整体失败。
 */
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

/**
 * @brief  销毁线程池实例并清理所有资源
 * @param  tp  ThreadPool*  要销毁的线程池指针，允许传入 NULL（空操作）
 * @return void
 *
 * @note   流程：设置 stop 标志 → broadcast 唤醒所有等待线程 → join 所有线程 →
 *         轮询并释放完成队列中残留 batch 的内存 → 销毁 mutex/cond → 释放线程池结构。
 *         残留 batch 仅释放内存，不再执行 process_fn 副作用。
 */
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

/**
 * @brief  向线程池提交一个 batch 去重任务
 * @param  tp     ThreadPool*  目标线程池指针，允许传入 NULL（返回 false）
 * @param  batch  TPBatch*     要提交的 batch 指针，允许传入 NULL（返回 false）
 * @return bool   返回 true 表示提交成功；false 表示工作队列已满（调用方应降级同步处理）
 *
 * @note   工作队列容量为 TP_QUEUE_CAPACITY（256）。队列满时不阻塞，直接返回 false。
 *         线程安全，内部自动加锁。
 */
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

/**
 * @brief  从完成队列中轮询取出一个已处理完毕的 batch
 * @param  tp  ThreadPool*  目标线程池指针，允许传入 NULL（返回 NULL）
 * @return TPBatch*  成功返回指向已完成 batch 的指针；队列为空时返回 NULL
 *
 * @note   本函数由主线程在 epoll 收到 eventfd 通知后调用。
 *         返回的 batch 内存由调用方负责释放（包括内部的 paths、stats、results 数组）。
 *         线程安全，内部自动加锁。
 */
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

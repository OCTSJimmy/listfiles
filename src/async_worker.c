/**
 * @file async_worker.c
 * @brief 异步输出工作线程实现
 *
 * 独立的后台线程负责将主循环批量提交的文件记录格式化并写入输出流。
 * 采用 mutex + cond 的生产者-消费者模型，支持批量 dequeue（一次性取出整个链表），
 * 将锁竞争降低至 1/256（ASYNC_BATCH_SIZE）。
 * 同时支持按行数切分输出文件（output_split_dir 模式）。
 */
#include "async_worker.h"
#include "output.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

/**
 * @brief  异步输出工作线程主函数
 * @param  arg  void*  指向 AsyncWorker 结构体的指针，不能为空
 * @return void*  始终返回 NULL
 *
 * @note   线程启动后进入循环：等待条件变量唤醒 → 批量取出任务链表 →
 *         串行调用 print_to_stream 写入文件 → 释放任务内存。
 *         当 stop 标志为 true 且任务链表为空时退出循环。
 *         退出前执行 fflush 确保数据落盘。
 */
static void *async_writer_thread(void *arg) {
    AsyncWorker *w = (AsyncWorker*)arg;
    while (1) {
        pthread_mutex_lock(&w->mutex);
        while (!w->stop && w->head == NULL) {
            pthread_cond_wait(&w->cond, &w->mutex);
        }
        if (w->stop && w->head == NULL) {
            pthread_mutex_unlock(&w->mutex);
            break;
        }
        
        /* 批量 dequeue：一次取出尽可能多的任务（降低 mutex 频率） */
        OutputTask *local_head = w->head;
        w->head = NULL;
        w->tail = NULL;
        pthread_mutex_unlock(&w->mutex);
        
        /* 串行处理本地链表 */
        OutputTask *task = local_head;
        while (task) {
            OutputTask *next = task->next;
            if (w->state->output_fp && !w->cfg->mute) {
                print_to_stream(w->cfg, w->state, task->path, &task->st, w->state->output_fp);
                w->state->output_line_count++;
                if (w->cfg->is_output_split_dir && w->state->output_line_count >= w->cfg->output_slice_lines) {
                    rotate_output_slice(w->cfg, w->state);
                }
            }
            free(task->path);
            free(task);
            task = next;
        }
    }
    if (w->state->output_fp && w->state->output_fp != stdout) {
        fflush(w->state->output_fp);
    }
    return NULL;
}

/**
 * @brief  初始化异步输出工作线程
 * @param  cfg    const Config*   全局配置指针，不能为空
 * @param  state  RuntimeState*   运行时状态指针，不能为空（用于访问 output_fp 等输出句柄）
 * @return AsyncWorker*  成功返回指向新分配工作线程控制结构的指针；内存不足时返回 NULL
 *
 * @note   内部创建独立 pthread，线程函数为 async_writer_thread。
 *         调用方需在程序结束前调用 async_worker_shutdown 进行清理。
 */
AsyncWorker* async_worker_init(const Config *cfg, RuntimeState *state) {
    AsyncWorker *w = calloc(1, sizeof(AsyncWorker));
    if (!w) return NULL;
    w->cfg = cfg;
    w->state = state;
    pthread_mutex_init(&w->mutex, NULL);
    pthread_cond_init(&w->cond, NULL);
    pthread_create(&w->thread, NULL, async_writer_thread, w);
    return w;
}

/**
 * @brief  关闭异步输出工作线程并释放所有资源
 * @param  w  AsyncWorker*  要关闭的工作线程指针，允许传入 NULL（空操作）
 * @return void
 *
 * @note   流程：设置 stop 标志 → 发送 cond 信号唤醒线程 → pthread_join 等待线程结束 →
 *         释放链表中残留的任务内存 → 销毁 mutex/cond → 释放控制结构。
 *         若链表中仍有未处理任务，会被静默丢弃（仅在 mute 模式下可能发生）。
 */
void async_worker_shutdown(AsyncWorker *w) {
    if (!w) return;
    pthread_mutex_lock(&w->mutex);
    w->stop = true;
    pthread_cond_signal(&w->cond);
    pthread_mutex_unlock(&w->mutex);
    pthread_join(w->thread, NULL);

    OutputTask *t = w->head;
    while (t) {
        OutputTask *next = t->next;
        free(t->path);
        free(t);
        t = next;
    }
    pthread_mutex_destroy(&w->mutex);
    pthread_cond_destroy(&w->cond);
    free(w);
}

/**
 * @brief  提交单个输出任务到异步工作线程
 * @param  w     AsyncWorker*       目标工作线程指针，允许传入 NULL（空操作）
 * @param  path  const char*        文件路径字符串，不能为空
 * @param  st    const struct stat* 文件 stat 信息指针，不能为空
 * @return void
 *
 * @note   内部复制 path 字符串和 st 结构体内容到任务节点，原数据可立即释放。
 *         通过 cond 信号唤醒工作线程。线程安全。
 */
void async_writer_submit(AsyncWorker *w, const char *path, const struct stat *st) {
    if (!w || !path) return;
    OutputTask *task = calloc(1, sizeof(OutputTask));
    if (!task) return;
    task->path = strdup(path);
    task->st = *st;

    pthread_mutex_lock(&w->mutex);
    if (w->tail) {
        w->tail->next = task;
    } else {
        w->head = task;
    }
    w->tail = task;
    pthread_cond_signal(&w->cond);
    pthread_mutex_unlock(&w->mutex);
}

/**
 * @brief  批量提交输出任务到异步工作线程
 * @param  w      AsyncWorker*  目标工作线程指针，允许传入 NULL（空操作）
 * @param  batch  OutputBatch*  批量任务结构体指针，允许传入 NULL 或 count==0（空操作）
 * @return void
 *
 * @note   将 batch 链表中的 head→tail 一次性接入工作线程队列，仅触发一次 mutex lock 和 cond signal。
 *         提交后自动清空 batch 结构（head=tail=NULL, count=0），所有权转移给工作线程。
 *         这是主循环推荐使用的提交方式，可显著降低锁竞争。
 */
void async_writer_submit_batch(AsyncWorker *w, OutputBatch *batch) {
    if (!w || !batch || batch->count == 0) return;
    
    pthread_mutex_lock(&w->mutex);
    if (w->tail) {
        w->tail->next = batch->head;
    } else {
        w->head = batch->head;
    }
    w->tail = batch->tail;
    
    pthread_cond_signal(&w->cond);
    pthread_mutex_unlock(&w->mutex);
    
    batch->head = NULL;
    batch->tail = NULL;
    batch->count = 0;
}

/**
 * @file async_worker.c
 * @brief 异步输出工作线程实现
 *
 * 独立的后台线程负责将主循环批量提交的文件记录格式化并写入输出流。
 * 采用 mutex + cond 的生产者-消费者模型，支持批量 dequeue（一次性取出整个链表），
 * 将锁竞争降低至 1/256（ASYNC_BATCH_SIZE）。
 * 同时支持按行数切分输出文件（output_split_dir 模式）。
 *
 * v15.6.0（P0-002 输出三态）：队列改为 OutputBatch 节点链，保留批次边界——
 * 每写完一个 batch 即 fflush（写入 OS 缓冲视为 OUTPUT_COMMITTED，不 fsync），
 * 随后按 batch 递减 output_pending[slot_id]，供主线程目录完成屏障判定。
 */
#include "async_worker.h"
#include "output.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

/**
 * @brief  异步输出工作线程主函数
 * @param  arg  void*  指向 AsyncWorker 结构体的指针，不能为空
 * @return void*  始终返回 NULL
 *
 * @note   线程启动后进入循环：等待条件变量唤醒 → 批量取出 batch 链表 →
 *         逐 batch 串行调用 print_to_stream 写入文件 → fflush → 递减 output_pending →
 *         释放任务与 batch 节点内存。
 *         当 stop 标志为 true 且 batch 链表为空时退出循环。
 *         退出前执行最终 fflush 确保数据落盘。
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

        /* 批量 dequeue：一次取出所有待写 batch（降低 mutex 频率） */
        OutputBatch *local_batch = w->head;
        w->head = NULL;
        w->tail = NULL;
        pthread_mutex_unlock(&w->mutex);

        /* 逐 batch 串行处理 */
        while (local_batch) {
            OutputBatch *next_batch = local_batch->next;
            OutputTask *task = local_batch->head;
            while (task) {
                OutputTask *next = task->next;
                if (w->state->output_fp) {
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
            /* v15.6.0（P0-002）：batch 写完即 fflush——写入 OS 缓冲即算 OUTPUT_COMMITTED
             * （设计允许，不做 fsync）；随后按 batch 递减 output_pending，
             * 目录完成屏障（main_loop.c advance_task_barriers）等待其归零才写 dpbin。 */
            if (w->state->output_fp) {
                fflush(w->state->output_fp);
            }
            if (local_batch->slot_id >= 0 && w->output_pending) {
                atomic_fetch_sub(&w->output_pending[local_batch->slot_id], 1);
                /* v15.6.0: 唤醒主线程及时推进完成屏障——否则主线程 cond_wait
                 * 100ms 粒度会拖住 Worker 再派发（小目录场景吞吐骤降） */
                if (w->main_cond) pthread_cond_signal(w->main_cond);
            }
            free(local_batch);
            local_batch = next_batch;
        }
    }
    if (w->state->output_fp && w->state->output_fp != stdout) {
        fflush(w->state->output_fp);
    }
    return NULL;
}

/**
 * @brief  初始化异步输出工作线程
 * @param  cfg             const Config*    全局配置指针，不能为空
 * @param  state           RuntimeState*    运行时状态指针，不能为空（用于访问 output_fp 等输出句柄）
 * @param  output_pending  _Atomic long*    v15.6.0: 每 slot 未 COMMITTED 输出 batch 计数数组（P0-002），允许 NULL（不计数）
 * @return AsyncWorker*  成功返回指向新分配工作线程控制结构的指针；内存不足时返回 NULL
 *
 * @note   内部创建独立 pthread，线程函数为 async_writer_thread。
 *         调用方需在程序结束前调用 async_worker_shutdown 进行清理。
 */
AsyncWorker* async_worker_init(const Config *cfg, RuntimeState *state, _Atomic long *output_pending,
                               pthread_cond_t *main_cond) {
    AsyncWorker *w = calloc(1, sizeof(AsyncWorker));
    if (!w) return NULL;
    w->cfg = cfg;
    w->state = state;
    w->output_pending = output_pending;
    w->main_cond = main_cond;
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
 *         释放链表中残留的 batch 与任务内存 → 销毁 mutex/cond → 释放控制结构。
 *         若链表中仍有未处理任务，会被静默丢弃（仅在 mute 模式下可能发生）。
 */
void async_worker_shutdown(AsyncWorker *w) {
    if (!w) return;
    pthread_mutex_lock(&w->mutex);
    w->stop = true;
    pthread_cond_signal(&w->cond);
    pthread_mutex_unlock(&w->mutex);
    pthread_join(w->thread, NULL);

    OutputBatch *b = w->head;
    while (b) {
        OutputBatch *next_batch = b->next;
        OutputTask *t = b->head;
        while (t) {
            OutputTask *next = t->next;
            free(t->path);
            free(t);
            t = next;
        }
        free(b);
        b = next_batch;
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
 *         包装为 slot_id=-1 的单任务 batch（无任务归属，不计 output_pending）。
 */
void async_writer_submit(AsyncWorker *w, const char *path, const struct stat *st) {
    if (!w || !path) return;
    OutputTask *task = calloc(1, sizeof(OutputTask));
    if (!task) return;
    task->path = strdup(path);
    task->st = *st;

    OutputBatch batch = {0};
    batch.head = task;
    batch.tail = task;
    batch.count = 1;
    batch.slot_id = -1;
    async_writer_submit_batch(w, &batch);
}

/**
 * @brief  批量提交输出任务到异步工作线程
 * @param  w      AsyncWorker*  目标工作线程指针，允许传入 NULL（空操作）
 * @param  batch  OutputBatch*  批量任务结构体指针，允许传入 NULL 或 count==0（空操作）
 * @return void
 *
 * @note   v15.6.0：为保留批次边界（按批 fflush + output_pending 计数），
 *         将 batch 堆拷贝为队列节点接入工作线程的 batch 队列，仅触发一次 mutex lock 和 cond signal。
 *         提交后自动清空调用方 batch 结构（head=tail=NULL, count=0），任务所有权转移给工作线程。
 *         这是主循环推荐使用的提交方式，可显著降低锁竞争。
 */
void async_writer_submit_batch(AsyncWorker *w, OutputBatch *batch) {
    if (!w || !batch || batch->count == 0) return;

    OutputBatch *node = malloc(sizeof(OutputBatch));
    if (!node) {
        /* OOM 兜底：同步写出并直接结算（fflush + output_pending 递减），
         * 保证与提交方的计数账目平衡，不丢条目、不卡完成屏障 */
        log_error("[AsyncWriter] batch node malloc failed (count=%d), fallback to sync write", batch->count);
        OutputTask *task = batch->head;
        while (task) {
            OutputTask *next = task->next;
            if (w->state->output_fp) {
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
        if (w->state->output_fp) {
            fflush(w->state->output_fp);
        }
        if (batch->slot_id >= 0 && w->output_pending) {
            atomic_fetch_sub(&w->output_pending[batch->slot_id], 1);
            if (w->main_cond) pthread_cond_signal(w->main_cond);
        }
        batch->head = NULL;
        batch->tail = NULL;
        batch->count = 0;
        return;
    }
    *node = *batch;
    node->next = NULL;

    pthread_mutex_lock(&w->mutex);
    if (w->tail) {
        w->tail->next = node;
    } else {
        w->head = node;
    }
    w->tail = node;

    pthread_cond_signal(&w->cond);
    pthread_mutex_unlock(&w->mutex);

    batch->head = NULL;
    batch->tail = NULL;
    batch->count = 0;
}

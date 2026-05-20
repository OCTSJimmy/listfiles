#ifndef DISPATCH_QUEUE_H
#define DISPATCH_QUEUE_H

#include <stddef.h>
#include <stdbool.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>

/**
 * @brief 派发任务条目
 *
 * Stage 3 (batch_processor) 生成，Stage 4 (main_loop) 消费。
 * 包含目录路径和 stat 信息，供 send_scan_to_ipc 和 dpbin_append 使用。
 */
typedef struct {
    char        *path;
    struct stat  st;
} DispatchTask;

/**
 * @brief Stage 3→4 派发队列
 *
 * 封装动态数组 + 互斥锁，所有并发访问内部处理。
 * 外部代码不直接接触 pthread_mutex_*。
 */
typedef struct {
    DispatchTask    *tasks;
    size_t           count;
    size_t           capacity;
    size_t           head;          /* v15.5.1: ring buffer head index for O(1) pop */
    pthread_mutex_t  mutex;
} DispatchQueue;

void dispatch_queue_init(DispatchQueue *q);
void dispatch_queue_destroy(DispatchQueue *q);

/**
 * @brief 将单个任务追加到队列（转移 path 所有权）
 * @return true 成功；false 队列满，path 未被接管，调用方需自行处理
 */
bool dispatch_queue_push(DispatchQueue *q, char *path, const struct stat *st);

/**
 * @brief 批量追加 backlog 数组（转移 path 所有权）
 * @note 调用后 backlog_paths 中的有效指针被移入队列，原数组内容被置 NULL
 */
void dispatch_queue_push_backlog(DispatchQueue *q, char **backlog_paths, struct stat *backlog_stats, int backlog_count);

/**
 * @brief 尝试弹出队列头部的任务
 * @param task_out 输出指针，成功时填充（path 由调用方负责 free）
 * @return true 成功弹出；false 队列为空
 */
bool dispatch_queue_pop(DispatchQueue *q, DispatchTask *task_out);

/**
 * @brief 压缩队列，移除内部 NULL 空洞
 */
void dispatch_queue_compact(DispatchQueue *q);

/**
 * @brief 获取当前任务数（调试用）
 */
size_t dispatch_queue_count(DispatchQueue *q);

#endif

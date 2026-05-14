#ifndef LOST_TASKS_H
#define LOST_TASKS_H

#include <stddef.h>
#include <stdbool.h>
#include <pthread.h>

/**
 * @brief 丢失任务队列
 *
 * 封装 lost_tasks 数组 + 互斥锁，所有并发访问内部处理。
 * 外部代码不直接接触 pthread_mutex_*。
 */
typedef struct {
    char           **tasks;
    size_t           count;
    size_t           capacity;
    pthread_mutex_t  mutex;
} LostTasksQueue;

void lost_tasks_init(LostTasksQueue *q);
void lost_tasks_destroy(LostTasksQueue *q);

/**
 * @brief 将单个 path 追加到队列（转移所有权）
 * @return true 成功；false 队列满，path 未被接管，调用方需自行处理
 */
bool lost_tasks_push(LostTasksQueue *q, char *path);

/**
 * @brief 批量追加 backlog 数组（转移所有权）
 * @note 调用后 backlog_paths 中的有效指针被移入队列，原数组内容被置 NULL
 */
void lost_tasks_push_backlog(LostTasksQueue *q, char **backlog_paths, int backlog_count);

/**
 * @brief 尝试弹出队列头部的任务
 * @param path_out 输出指针，成功时指向任务字符串（调用方负责 free 或发送）
 * @return true 成功弹出；false 队列为空
 */
bool lost_tasks_pop(LostTasksQueue *q, char **path_out);

/**
 * @brief 压缩队列，移除内部 NULL 空洞
 */
void lost_tasks_compact(LostTasksQueue *q);

/**
 * @brief 获取当前任务数（调试用）
 */
size_t lost_tasks_count(LostTasksQueue *q);

#endif

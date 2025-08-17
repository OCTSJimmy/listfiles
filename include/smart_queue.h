#ifndef SMART_QUEUE_H
#define SMART_QUEUE_H

#include "config.h"

// 初始化智能队列
void init_smart_queue(SmartQueue *queue);

// 智能入队
void smart_enqueue(const Config *cfg, SmartQueue *queue, const char *path, const struct stat *info);

// 智能出队
QueueEntry *smart_dequeue(const Config *cfg, SmartQueue *queue, RuntimeState *state);

// 清理智能队列，释放所有资源
void cleanup_smart_queue(SmartQueue *queue);
void add_to_buffer(SmartQueue *queue, QueueEntry *entry);
void add_to_active(SmartQueue *queue, QueueEntry *entry);
void flush_buffer_to_disk(const Config *cfg, SmartQueue *queue);
void load_batch_from_disk(const Config *cfg, SmartQueue *queue);
void append_buffer_to_active_O1(SmartQueue *queue);
void refill_active(const Config *cfg, SmartQueue *queue);

#endif // SMART_QUEUE_H
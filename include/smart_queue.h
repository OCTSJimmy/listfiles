#ifndef SMART_QUEUE_H
#define SMART_QUEUE_H

#include "config.h"

// 初始化智能队列
void init_smart_queue(SmartQueue *queue);

// 智能入队
void smart_enqueue(const Config *cfg, SmartQueue *queue, const char *path, const struct stat *info);
// 新增：盲入队 (不执行去重，仅入队)
void blind_enqueue(SmartQueue *queue, const char *path);
// 智能出队
ScanNode *smart_dequeue(const Config *cfg, SmartQueue *queue, RuntimeState *state);

// 清理智能队列，释放所有资源
void cleanup_smart_queue(SmartQueue *queue);
void add_to_buffer(SmartQueue *queue, ScanNode *entry);
void add_to_active(SmartQueue *queue, ScanNode *entry);
void flush_buffer_to_disk(const Config *cfg, SmartQueue *queue);
void load_batch_from_disk(const Config *cfg, SmartQueue *queue);
void append_buffer_to_active_O1(SmartQueue *queue);
void refill_active(const Config *cfg, SmartQueue *queue);
ScanNode* alloc_node(SmartQueue *queue);
void recycle_scan_node(SmartQueue *queue, ScanNode *node);


#endif // SMART_QUEUE_H
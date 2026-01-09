#ifndef TRAVERSAL_H
#define TRAVERSAL_H

#include "config.h"
#include "looper.h"
#include "monitor.h"

// 前置声明，避免循环引用
struct MessageQueue;
// === 本地结构 ===
// 恢复线程参数
typedef struct {
    const Config *cfg;
    RuntimeState *state;
    MessageQueue *target_mq;
} ResumeThreadArgs;

// [修改] Worker 参数增加 Monitor 上下文
typedef struct {
    const Config *cfg;
    Monitor *monitor; 
} WorkerArgs;

// 低优先级队列节点 (用于 Looper 暂存新任务)
typedef struct LowPriNode {
    char *path;
    struct LowPriNode *next;
} LowPriNode;

// 主遍历入口
void traverse_files(const Config *cfg, RuntimeState *state);

// 增加 pending 任务计数 (用于 progress 恢复等场景)
void traversal_add_pending_tasks(int count);

// [新增] 补充一个新的 Worker 线程 (供 Monitor 发现僵尸后调用)
// 当 Monitor 判定某个 Worker 卡死并将其标记为僵尸后，会调用此函数维持并发度
void traversal_spawn_replacement_worker(const Config *cfg, struct Monitor *monitor);
// [新增]
void traversal_notify_worker_abandoned(void);

#endif // TRAVERSAL_H
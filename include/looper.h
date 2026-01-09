#ifndef LOOPER_H
#define LOOPER_H

#include <pthread.h>
#include <stdbool.h>
#include <sys/stat.h>

// --- 1. 定义批处理包 (TaskBatch) ---
// 批量大小：权衡内存占用和锁竞争频率
#define BATCH_SIZE 64

typedef struct {
    int count;
    char *paths[BATCH_SIZE];       // 路径数组
    struct stat stats[BATCH_SIZE]; // 对应的 stat 信息 (避免重复 IO)
} TaskBatch;

// --- 2. 定义消息类型 ---
enum {
    MSG_QUIT = 0,
    
    // [Looper -> Worker] 指令
    MSG_SCAN_DIR,       // 扫描一个目录 (payload: char* path)
    MSG_CHECK_BATCH,    // 批量检查路径 (用于 resume, payload: TaskBatch*)
    
    // [Worker -> Looper] 汇报
    MSG_RESULT_BATCH,   // 汇报发现的子项 (payload: TaskBatch*)
    MSG_WORKER_STUCK,   // 报告卡死 (payload: char* path)
    
    // [新增] 任务状态与流控信号
    MSG_TASK_DONE,       // Worker 完成了一个原子任务 (payload: NULL)
    MSG_RESUME_FINISHED  // 恢复线程通知 Looper 恢复已完成 (payload: NULL)
};

// --- 3. 消息对象 (支持池化) ---
typedef struct Message {
    int what;
    void *obj;          // 负载数据 (Batch 或 Path)
    struct Message *next;
} Message;

// --- 4. 消息队列 ---
typedef struct {
    // 活跃消息队列
    Message *head;
    Message *tail;
    
    // 消息回收池 (Free List)
    Message *pool_head;
    int pool_count;
    
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    bool quitting;
} MessageQueue;

// --- 5. 接口声明 ---

// 队列生命周期
void mq_init(MessageQueue *mq);
void mq_destroy(MessageQueue *mq);

// 批次管理 (生产端/消费端通用)
TaskBatch* batch_create();
void batch_add(TaskBatch *batch, const char *path, const struct stat *st);
void batch_destroy(TaskBatch *batch); // 释放批次内所有资源

// 消息操作
// 从池中获取消息 (替代 malloc)
Message* mq_obtain(MessageQueue *mq, int what, void *obj);
// 发送消息 (入队)
void mq_enqueue(MessageQueue *mq, Message *msg);
// 快捷发送 (内部调用 mq_obtain)
void mq_send(MessageQueue *mq, int what, void *obj); 
// 取出消息 (阻塞等待)
Message* mq_dequeue(MessageQueue *mq);
// 回收消息到池 (替代 free)
void mq_recycle(MessageQueue *mq, Message *msg);

// [新增] 获取队列当前长度 (用于流控)
int mq_get_size(MessageQueue *mq);

#endif // LOOPER_H
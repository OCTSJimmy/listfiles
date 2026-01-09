#ifndef LOOPER_H
#define LOOPER_H

#include "config.h"
#include <pthread.h>
#include <sys/stat.h>

#define BATCH_SIZE 128

// 消息类型
#define MSG_SCAN_DIR    1
#define MSG_RESULT_BATCH 2
#define MSG_TASK_DONE   3
#define MSG_CHECK_BATCH 4
#define MSG_RESUME_FINISHED 5
#define MSG_WORKER_STUCK 6
#define MSG_STOP        999  // [新增] 停止信号

// ... [其余结构体保持不变] ...
// 务必保留 Message, TaskBatch, MessageQueue 的定义

typedef struct Message {
    int what;
    void *obj;
    struct Message *next;
} Message;

typedef struct {
    char *paths[BATCH_SIZE];
    struct stat stats[BATCH_SIZE];
    int count;
} TaskBatch;

typedef struct {
    Message *head;
    Message *tail;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    
    // 对象池
    Message *pool_head;
    int pool_count;
    
    bool quitting;
} MessageQueue;

// 函数声明
void mq_init(MessageQueue *mq);
void mq_enqueue(MessageQueue *mq, Message *msg);
void mq_send(MessageQueue *mq, int what, void *obj);
Message* mq_dequeue(MessageQueue *mq);
void mq_recycle(MessageQueue *mq, Message *msg);
void mq_destroy(MessageQueue *mq);

TaskBatch* batch_create();
void batch_add(TaskBatch *batch, const char *path, const struct stat *info);
void batch_destroy(TaskBatch *batch);

#endif // LOOPER_H
#include "looper.h"
#include "utils.h" // 需要 safe_malloc
#include <stdlib.h>
#include <string.h>

#define MAX_MSG_POOL_SIZE 100

// ==========================================
// 1. 批处理 (TaskBatch) 管理实现
// ==========================================

TaskBatch* batch_create() {
    TaskBatch *b = safe_malloc(sizeof(TaskBatch));
    b->count = 0;
    // 指针数组清零，方便后续安全释放
    memset(b->paths, 0, sizeof(b->paths));
    return b;
}

void batch_add(TaskBatch *batch, const char *path, const struct stat *info) {
    if (batch->count < BATCH_SIZE) {
        batch->paths[batch->count] = strdup(path); // 复制路径字符串
        if (info) {
            batch->stats[batch->count] = *info;    // 复制 stat 结构体
        } else {
            memset(&batch->stats[batch->count], 0, sizeof(struct stat));
        }
        batch->count++;
    }
}

void batch_destroy(TaskBatch *batch) {
    if (!batch) return;
    for (int i = 0; i < batch->count; i++) {
        if (batch->paths[i]) {
            free(batch->paths[i]); // 释放 strdup 出来的路径
        }
    }
    free(batch); // 释放结构体本身
}

// ==========================================
// 2. 消息队列与对象池实现
// ==========================================

void mq_init(MessageQueue *mq) {
    memset(mq, 0, sizeof(MessageQueue));
    mq->head = NULL;
    mq->tail = NULL;
    mq->pool_head = NULL;
    mq->pool_count = 0;
    mq->quitting = false;
    pthread_mutex_init(&mq->mutex, NULL);
    pthread_cond_init(&mq->cond, NULL);
}

Message* mq_obtain(MessageQueue *mq, int what, void *obj) {
    Message *msg = NULL;

    pthread_mutex_lock(&mq->mutex);
    if (mq->pool_head) {
        // 1. 命中缓存：从回收池头部取一个
        msg = mq->pool_head;
        mq->pool_head = msg->next;
        mq->pool_count--;
    }
    pthread_mutex_unlock(&mq->mutex);

    // 2. 未命中：申请新内存
    if (!msg) {
        msg = safe_malloc(sizeof(Message));
    }

    // 3. 重置状态
    msg->what = what;
    msg->obj = obj;
    msg->next = NULL;
    
    return msg;
}

void mq_recycle(MessageQueue *mq, Message *msg) {
    if (!msg) return;

    pthread_mutex_lock(&mq->mutex);
    if (mq->pool_count < MAX_MSG_POOL_SIZE) {
        // 1. 池未满：头插法放回池子
        msg->next = mq->pool_head;
        mq->pool_head = msg;
        mq->pool_count++;
        // 重要：断开对外部数据的引用，防止野指针，但并不释放外部数据
        msg->obj = NULL; 
    } else {
        // 2. 池满了：直接释放内存
        free(msg);
    }
    pthread_mutex_unlock(&mq->mutex);
}

void mq_enqueue(MessageQueue *mq, Message *msg) {
    pthread_mutex_lock(&mq->mutex);
    if (mq->quitting) {
        // 如果队列已退出，直接销毁消息，防止内存泄漏
        // 注意：这里我们只能释放 Message 容器。
        // 如果 msg->obj 是 TaskBatch，这里会泄漏 Batch 的内容。
        // 所以调用者应该在销毁 mq 前确保停止生产。
        free(msg); 
        pthread_mutex_unlock(&mq->mutex);
        return;
    }
    
    if (mq->tail) {
        mq->tail->next = msg;
        mq->tail = msg;
    } else {
        mq->head = mq->tail = msg;
        mq->head = msg; // 修正逻辑：head 和 tail 指向同一个
    }
    pthread_cond_signal(&mq->cond);
    pthread_mutex_unlock(&mq->mutex);
}

void mq_send(MessageQueue *mq, int what, void *obj) {
    Message *msg = mq_obtain(mq, what, obj);
    mq_enqueue(mq, msg);
}

Message* mq_dequeue(MessageQueue *mq) {
    pthread_mutex_lock(&mq->mutex);
    while (mq->head == NULL && !mq->quitting) {
        pthread_cond_wait(&mq->cond, &mq->mutex);
    }

    if (mq->quitting && mq->head == NULL) {
        pthread_mutex_unlock(&mq->mutex);
        return NULL;
    }

    Message *msg = mq->head;
    mq->head = msg->next;
    if (mq->head == NULL) mq->tail = NULL;
    
    msg->next = NULL; // 断开链表连接
    pthread_mutex_unlock(&mq->mutex);
    return msg;
}

// 【完善的清理逻辑】
void mq_destroy(MessageQueue *mq) {
    pthread_mutex_lock(&mq->mutex);
    mq->quitting = true;
    pthread_cond_broadcast(&mq->cond); // 唤醒所有等待者让它们退出
    
    // 1. 清理活跃队列中剩余的消息 (未被消费的)
    Message *curr = mq->head;
    while (curr) {
        Message *next = curr->next;
        // 警告：这里只释放 Message 结构体。
        // 如果 msg->obj 指向堆内存（如 strdup 的字符串或 TaskBatch），这里无法自动释放。
        // 在 C 语言泛型容器中，这通常需要用户保证 destroy 时队列已空，
        // 或者提供一个 free_func 回调。这里保持简单，仅防 Message 泄漏。
        free(curr);
        curr = next;
    }
    mq->head = mq->tail = NULL;

    // 2. 清理消息回收池
    curr = mq->pool_head;
    while (curr) {
        Message *next = curr->next;
        free(curr);
        curr = next;
    }
    mq->pool_head = NULL;
    mq->pool_count = 0;

    pthread_mutex_unlock(&mq->mutex);
    
    pthread_mutex_destroy(&mq->mutex);
    pthread_cond_destroy(&mq->cond);
}
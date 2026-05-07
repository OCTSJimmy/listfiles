#include "looper.h"
#include "utils.h" 
#include <stdlib.h>
#include <string.h>
#include <stdio.h> // for fprintf

#define MAX_MSG_POOL_SIZE 100

// ==========================================
// 1. 批处理 (TaskBatch) 管理实现
// ==========================================

TaskBatch* batch_create() {
    TaskBatch *b = safe_malloc(sizeof(TaskBatch));
    b->count = 0;
    memset(b->paths, 0, sizeof(b->paths));
    return b;
}

void batch_add(TaskBatch *batch, const char *path, const struct stat *info) {
    if (batch->count < BATCH_SIZE) {
        batch->paths[batch->count] = strdup(path); // 复制路径字符串
        if (info) {
            batch->stats[batch->count] = *info;
        } else {
            memset(&batch->stats[batch->count], 0, sizeof(struct stat));
        }
        batch->count++;
    }
}

// [修复] 释放 Batch 内的所有路径内存
void batch_destroy(TaskBatch *batch) {
    if (!batch) return;
    for (int i = 0; i < batch->count; i++) {
        if (batch->paths[i]) {
            free(batch->paths[i]);
        }
    }
    free(batch);
}

// ==========================================
// 2. 消息队列实现
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
        msg = mq->pool_head;
        mq->pool_head = msg->next;
        mq->pool_count--;
    }
    pthread_mutex_unlock(&mq->mutex);

    if (!msg) msg = safe_malloc(sizeof(Message));
    msg->what = what;
    msg->obj = obj;
    msg->next = NULL;
    return msg;
}

void mq_recycle(MessageQueue *mq, Message *msg) {
    if (!msg) return;
    pthread_mutex_lock(&mq->mutex);
    if (mq->pool_count < MAX_MSG_POOL_SIZE) {
        msg->next = mq->pool_head;
        mq->pool_head = msg;
        mq->pool_count++;
        msg->obj = NULL; 
    } else {
        free(msg);
    }
    pthread_mutex_unlock(&mq->mutex);
}

void mq_enqueue(MessageQueue *mq, Message *msg) {
    pthread_mutex_lock(&mq->mutex);
    if (mq->quitting) {
        free(msg); 
        pthread_mutex_unlock(&mq->mutex);
        return;
    }
    
    if (mq->tail) {
        mq->tail->next = msg;
        mq->tail = msg;
    } else {
        mq->head = mq->tail = msg;
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
    
    msg->next = NULL;
    pthread_mutex_unlock(&mq->mutex);
    return msg;
}

void mq_destroy(MessageQueue *mq) {
    pthread_mutex_lock(&mq->mutex);
    mq->quitting = true;
    pthread_cond_broadcast(&mq->cond);
    
    Message *curr = mq->head;
    while (curr) {
        Message *next = curr->next;
        free(curr); // 注意：这里可能泄露 msg->obj，但在退出阶段可接受
        curr = next;
    }
    mq->head = mq->tail = NULL;

    curr = mq->pool_head;
    while (curr) {
        Message *next = curr->next;
        free(curr);
        curr = next;
    }
    mq->pool_head = NULL;
    pthread_mutex_unlock(&mq->mutex);
    
    pthread_mutex_destroy(&mq->mutex);
    pthread_cond_destroy(&mq->cond);
}
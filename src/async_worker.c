#include "async_worker.h"
#include "utils.h"
#include "output.h"
#include "progress.h"
#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h> // 确保 stat 相关函数有定义

// 简单的链表队列节点
typedef struct AsyncNode {
    char *path;
    struct AsyncNode *next;
} AsyncNode;

static struct {
    AsyncNode *head, *tail;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    bool stop_flag;
    
    // 批处理计数器
    unsigned long pending_count;

    // === 新增：队列计数器 ===
    size_t queue_count; // <--- 新增这个变量来维护积压数量
    
    const Config *cfg;
    RuntimeState *state;
} g_worker;

#define BATCH_FLUSH_SIZE 5000 

// Getter 实现：获取当前异步队列的堆积数量
size_t async_worker_get_queue_size() {
    // 加锁读取，保证读到的是即时准确值，且防止32位机器上的读写撕裂
    pthread_mutex_lock(&g_worker.mutex);
    size_t size = g_worker.queue_count;
    pthread_mutex_unlock(&g_worker.mutex);
    return size;
}

// 工作线程函数：消费者
static void *worker_thread_func(void *arg) {
    (void)arg;
    
    while (true) {
        AsyncNode *node = NULL;

        // 1. 加锁获取队列项
        pthread_mutex_lock(&g_worker.mutex);
        while (g_worker.head == NULL && !g_worker.stop_flag) {
            pthread_cond_wait(&g_worker.cond, &g_worker.mutex);
        }

        if (g_worker.head != NULL) {
            node = g_worker.head;
            g_worker.head = node->next;
            if (g_worker.head == NULL) g_worker.tail = NULL;
            
            // === 维护计数：出队减一 ===
            g_worker.queue_count--; // <--- 关键点：在锁内递减
            
        } else if (g_worker.stop_flag) {
            pthread_mutex_unlock(&g_worker.mutex);
            break; // 退出循环
        }
        pthread_mutex_unlock(&g_worker.mutex);

        if (!node) continue;

        // 2. 执行延迟的 lstat (Lazy Stat)
        struct stat info;
        bool stat_success = false;
        
        // 只有当用户需要元数据时，才真正执行 lstat
        if (g_worker.cfg->size || g_worker.cfg->user || g_worker.cfg->mtime || 
            g_worker.cfg->group || g_worker.cfg->atime) {
            if (lstat(node->path, &info) == 0) {
                stat_success = true;
            } else {
                // lstat 失败，可能文件被删，释放内存跳过
                free(node->path);
                free(node);
                continue; 
            }
        } else {
            // 不需要元数据
            memset(&info, 0, sizeof(info));
            stat_success = true; 
        }

        // 3. 格式化并输出
        if (stat_success) {
            // 注意：RuntimeState 中的 file_count 在这里更新
            // 如果是多线程Writer，这里需要原子操作或加锁，但目前是单Writer，所以安全
            g_worker.state->file_count++; 
            
            format_output(g_worker.cfg, g_worker.state, node->path, &info);
            g_worker.pending_count++;
        }

        free(node->path);
        free(node);

        // 4. 批量落盘
        if (g_worker.pending_count >= BATCH_FLUSH_SIZE) {
            FILE *fp = g_worker.state->output_fp ? g_worker.state->output_fp : stdout;
            fflush(fp);
            
            if (g_worker.cfg->continue_mode) {
                 atomic_update_index(g_worker.cfg, g_worker.state);
            }
            g_worker.pending_count = 0;
        }
    }
    
    // 循环结束后处理剩余缓冲
    FILE *fp = g_worker.state->output_fp ? g_worker.state->output_fp : stdout;
    fflush(fp);
    if (g_worker.cfg->continue_mode) {
         atomic_update_index(g_worker.cfg, g_worker.state);
    }
    
    return NULL;
}

void async_worker_init(const Config *cfg, RuntimeState *state) {
    g_worker.head = g_worker.tail = NULL;
    g_worker.stop_flag = false;
    g_worker.pending_count = 0;
    
    // === 初始化计数器 ===
    g_worker.queue_count = 0; // <--- 初始化
    
    g_worker.cfg = cfg;
    g_worker.state = state;
    
    pthread_mutex_init(&g_worker.mutex, NULL);
    pthread_cond_init(&g_worker.cond, NULL);
    
    pthread_t tid;
    pthread_create(&tid, NULL, worker_thread_func, NULL);
    pthread_detach(tid); 
}

void async_worker_push_file(const char *path) {
    AsyncNode *node = safe_malloc(sizeof(AsyncNode));
    node->path = strdup(path); 
    node->next = NULL;

    pthread_mutex_lock(&g_worker.mutex);
    if (g_worker.tail) {
        g_worker.tail->next = node;
        g_worker.tail = node;
    } else {
        g_worker.head = g_worker.tail = node;
    }
    
    // === 维护计数：入队加一 ===
    g_worker.queue_count++; // <--- 关键点：在锁内递增
    
    pthread_cond_signal(&g_worker.cond);
    pthread_mutex_unlock(&g_worker.mutex);
}

void async_worker_shutdown() {
    pthread_mutex_lock(&g_worker.mutex);
    g_worker.stop_flag = true;
    pthread_cond_signal(&g_worker.cond);
    pthread_mutex_unlock(&g_worker.mutex);
    sleep(1); 
}
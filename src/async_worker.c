#include "async_worker.h"
#include "utils.h"
#include "output.h"
#include "progress.h"
#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h> // for gettimeofday
#include <time.h>     // for timespec, clock_gettime
#include <errno.h>    // for ETIMEDOUT

// ... (结构体定义保持不变) ...

typedef struct AsyncNode {
    char *path;
    struct AsyncNode *next;
} AsyncNode;

static struct {
    AsyncNode *head, *tail;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    bool stop_flag;
    
    unsigned long pending_count;
    size_t queue_count;
    
    // === 新增：上次落盘时间 ===
    time_t last_flush_time; // <--- 关键变量
    
    const Config *cfg;
    RuntimeState *state;
} g_worker;

#define BATCH_FLUSH_SIZE 5000 
#define FLUSH_INTERVAL_SEC 5 // 强制落盘时间间隔

// 辅助函数：执行刷盘逻辑
static void perform_flush() {
    if (g_worker.pending_count > 0) {
        FILE *fp = g_worker.state->output_fp ? g_worker.state->output_fp : stdout;
        fflush(fp);
        
        // 只有在真的写入了数据时，才更新索引
        if (g_worker.cfg->continue_mode) {
             atomic_update_index(g_worker.cfg, g_worker.state);
        }
        
        g_worker.pending_count = 0;
        g_worker.last_flush_time = time(NULL); // 更新时间戳
    } else {
        // 即使没有数据写入，也可以更新一下时间戳，避免空转时的逻辑判断干扰
        g_worker.last_flush_time = time(NULL);
    }
}

// 辅助函数：计算下一次超时的绝对时间
static void get_next_timeout(struct timespec *ts) {
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += FLUSH_INTERVAL_SEC;
}

// Getter (保持不变)
size_t async_worker_get_queue_size() {
    pthread_mutex_lock(&g_worker.mutex);
    size_t size = g_worker.queue_count;
    pthread_mutex_unlock(&g_worker.mutex);
    return size;
}

// 工作线程函数：消费者
static void *worker_thread_func(void *arg) {
    (void)arg;
    
    // 初始化上次刷盘时间
    g_worker.last_flush_time = time(NULL);

    while (true) {
        AsyncNode *node = NULL;

        pthread_mutex_lock(&g_worker.mutex);
        
        // === 循环等待逻辑修改 ===
        while (g_worker.head == NULL && !g_worker.stop_flag) {
            // 不再死等，而是计算出一个超时时间点
            // 我们希望每隔 FLUSH_INTERVAL_SEC 至少醒来一次检查刷盘
            
            struct timespec ts;
            // 获取当前时间
            clock_gettime(CLOCK_REALTIME, &ts);
            
            // 计算截止时间：上次刷盘时间 + 5秒
            // 注意：这里不能简单地用 current + 5，因为如果处理任务耗时了4秒，我们只应再睡1秒
            time_t now = time(NULL);
            time_t next_flush = g_worker.last_flush_time + FLUSH_INTERVAL_SEC;
            
            if (next_flush <= now) {
                // 已经超时了，不要睡了，直接去刷盘（通过跳出循环）
                break; 
            } else {
                ts.tv_sec = next_flush; // 设置绝对截止时间
            }

            int rc = pthread_cond_timedwait(&g_worker.cond, &g_worker.mutex, &ts);
            
            if (rc == ETIMEDOUT) {
                // 超时醒来，说明这段时间没有新数据，或者数据来得太慢
                // 跳出循环，去执行刷盘检查
                break;
            }
        }

        // 取出节点
        if (g_worker.head != NULL) {
            node = g_worker.head;
            g_worker.head = node->next;
            if (g_worker.head == NULL) g_worker.tail = NULL;
            g_worker.queue_count--; 
        } else if (g_worker.stop_flag) {
            // 收到退出信号
            pthread_mutex_unlock(&g_worker.mutex);
            break; 
        }
        
        // 即使 queue 为空 (超时醒来)，我们也会走到这里，此时 node 为 NULL
        pthread_mutex_unlock(&g_worker.mutex);

        // 如果取到了任务，执行处理
        if (node) {
            struct stat info;
            bool stat_success = false;
            
            if (g_worker.cfg->size || g_worker.cfg->user || g_worker.cfg->mtime || 
                g_worker.cfg->group || g_worker.cfg->atime) {
                if (lstat(node->path, &info) == 0) {
                    stat_success = true;
                } else {
                    free(node->path);
                    free(node);
                    // 注意：这里continue前也要检查时间，为了代码简洁，我们在下面统一检查
                    goto check_flush; 
                }
            } else {
                memset(&info, 0, sizeof(info));
                stat_success = true; 
            }

            if (stat_success) {
                g_worker.state->file_count++; 
                format_output(g_worker.cfg, g_worker.state, node->path, &info);
                g_worker.pending_count++;
            }
            
            free(node->path);
            free(node);
        }

check_flush:
        // === 双轨并行检测 ===
        // 1. 数量检测
        bool need_flush_by_count = (g_worker.pending_count >= BATCH_FLUSH_SIZE);
        
        // 2. 时间检测
        time_t now = time(NULL);
        bool need_flush_by_time = (g_worker.pending_count > 0 && 
                                  (now - g_worker.last_flush_time >= FLUSH_INTERVAL_SEC));

        if (need_flush_by_count || need_flush_by_time) {
            perform_flush();
        }
    }
    
    // 退出前的最后清理
    perform_flush();
    
    return NULL;
}

void async_worker_init(const Config *cfg, RuntimeState *state) {
    g_worker.head = g_worker.tail = NULL;
    g_worker.stop_flag = false;
    g_worker.pending_count = 0;
    g_worker.queue_count = 0;
    g_worker.last_flush_time = time(NULL); // 初始化
    
    g_worker.cfg = cfg;
    g_worker.state = state;
    
    pthread_mutex_init(&g_worker.mutex, NULL);
    
    // 注意：使用 timedwait 通常建议使用 CLOCK_MONOTONIC，
    // 但 pthread_cond_timedwait 默认使用 CLOCK_REALTIME (系统时间)。
    // 如果系统时间被修改，可能会影响等待时长，但在这种日志刷盘场景下影响不大。
    // 如需严格单调时间，需要设置 cond attr，这里保持默认即可。
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
    g_worker.queue_count++;
    
    pthread_cond_signal(&g_worker.cond);
    pthread_mutex_unlock(&g_worker.mutex);
}

void async_worker_shutdown() {
    pthread_mutex_lock(&g_worker.mutex);
    g_worker.stop_flag = true;
    // 发送信号唤醒可能在 timedwait 的线程
    pthread_cond_signal(&g_worker.cond);
    pthread_mutex_unlock(&g_worker.mutex);
    sleep(1); 
}
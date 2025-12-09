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

static struct {
    WriteNode *head, *tail;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    bool stop_flag;
    unsigned long pending_count;
    size_t queue_count;
    time_t last_flush_time;
    const Config *cfg;
    RuntimeState *state;
} g_worker;

// 辅助：执行刷盘
// 注意：现在这个函数只刷 Output，不刷 Progress (Progress 由 Checkpoint 触发)
static void perform_flush_output() {
    if (g_worker.pending_count > 0) {
        FILE *fp = g_worker.state->output_fp ? g_worker.state->output_fp : stdout;
        fflush(fp);
        g_worker.pending_count = 0;
        g_worker.last_flush_time = time(NULL);
    }
}

// 辅助：执行进度保存 (原子更新)
static void perform_save_progress(const ProgressSnapshot *snap) {
    // 我们需要一个特殊的 atomic_update 变体，或者临时修改 state 再调用
    // 为了安全，我们构造一个临时的 state 副本传给 atomic_update_index
    // 但 atomic_update_index 内部使用了 state 的指针。
    // 这里我们直接利用 snap 的值写入磁盘，重写 atomic_update_index 的逻辑部分
    
    char *idx_file = get_index_filename(g_worker.cfg->progress_base);
    char *tmp_file = safe_malloc(strlen(idx_file) + 5);
    snprintf(tmp_file, strlen(idx_file) + 5, "%s.tmp", idx_file);
    
    FILE *tmp_fp = fopen(tmp_file, "w");
    if (tmp_fp) {
        // 使用快照中的值，而不是 g_worker.state (后者可能已经变了)
        fprintf(tmp_fp, "%lu %lu %lu %lu %lu\n", 
                snap->process_slice_index, 
                snap->processed_count,
                snap->write_slice_index,
                snap->output_slice_num,
                snap->output_line_count);
        fclose(tmp_fp);
        
        if (rename(tmp_file, idx_file) != 0) {
            perror("Worker: 无法更新进度索引");
        }
    }
    free(idx_file);
    free(tmp_file);
}

// Getter (保持不变)
size_t async_worker_get_queue_size() {
    pthread_mutex_lock(&g_worker.mutex);
    size_t size = g_worker.queue_count;
    pthread_mutex_unlock(&g_worker.mutex);
    return size;
}

static void *worker_thread_func(void *arg) {
    (void)arg;
    g_worker.last_flush_time = time(NULL);

    while (true) {
        WriteNode *node = NULL;
        pthread_mutex_lock(&g_worker.mutex);
        
        while (g_worker.head == NULL && !g_worker.stop_flag) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            time_t next_flush = g_worker.last_flush_time + FLUSH_INTERVAL_SEC;
            time_t now = time(NULL);
            if (next_flush <= now) break; 
            ts.tv_sec = next_flush;
            
            int rc = pthread_cond_timedwait(&g_worker.cond, &g_worker.mutex, &ts);
            if (rc == ETIMEDOUT) break;
        }

        if (g_worker.head != NULL) {
            node = g_worker.head;
            g_worker.head = node->next;
            if (g_worker.head == NULL) g_worker.tail = NULL;
            g_worker.queue_count--; 
        } else if (g_worker.stop_flag) {
            pthread_mutex_unlock(&g_worker.mutex);
            break; 
        }
        pthread_mutex_unlock(&g_worker.mutex);

        if (node) {
            if (node->type == NODE_TYPE_FILE) {
                // === 处理文件 ===
                struct stat info;
                bool stat_success = false;
                
                // 只有需要元数据时才 lstat
                if (g_worker.cfg->size || g_worker.cfg->user || g_worker.cfg->mtime || 
                    g_worker.cfg->group || g_worker.cfg->atime || g_worker.cfg->format) {
                    if (lstat(node->path, &info) == 0) stat_success = true;
                } else {
                    memset(&info, 0, sizeof(info));
                    stat_success = true; 
                }

                if (stat_success) {
                    g_worker.state->file_count++; // Worker 更新文件产出计数
                    format_output(g_worker.cfg, g_worker.state, node->path, &info);
                    g_worker.pending_count++;
                }
                free(node->path);

            } else if (node->type == NODE_TYPE_CHECKPOINT) {
                // === 处理检查点 (进度3) ===
                // 1. 先强制刷盘 Output，确保之前的文件物理落盘
                perform_flush_output();
                
                // 2. 然后保存进度索引
                perform_save_progress(&node->progress);
                
                // verbose_printf(g_worker.cfg, 2, "Checkpoint saved: slice %lu, count %lu\n", 
                //                node->progress.process_slice_index, node->progress.processed_count);
            }
            free(node->path);
            free(node);
        }

        // 自动刷盘逻辑 (仅针对 Output)
        time_t now = time(NULL);
        if (g_worker.pending_count >= BATCH_FLUSH_SIZE || 
           (g_worker.pending_count > 0 && now - g_worker.last_flush_time >= FLUSH_INTERVAL_SEC)) {
            perform_flush_output();
        }
    }
    
    perform_flush_output();
    return NULL;
}

void async_worker_init(const Config *cfg, RuntimeState *state) {
    g_worker.head = g_worker.tail = NULL;
    g_worker.stop_flag = false;
    g_worker.pending_count = 0;
    g_worker.queue_count = 0;
    g_worker.last_flush_time = time(NULL);
    g_worker.cfg = cfg;
    g_worker.state = state;
    pthread_mutex_init(&g_worker.mutex, NULL);
    pthread_cond_init(&g_worker.cond, NULL);
    
    pthread_t tid;
    pthread_create(&tid, NULL, worker_thread_func, NULL);
    pthread_detach(tid); 
}

void push_write_task_file(const char *path) {
    WriteNode *node = safe_malloc(sizeof(WriteNode));
    node->type = NODE_TYPE_FILE;
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
void push_write_task_checkpoint(const RuntimeState *current_state) {
    WriteNode *node = safe_malloc(sizeof(WriteNode));
    node->type = NODE_TYPE_CHECKPOINT;
    node->path = NULL;
    
    // 捕获当前的进度状态快照
    node->progress.process_slice_index = current_state->process_slice_index;
    node->progress.processed_count     = current_state->processed_count;
    node->progress.write_slice_index   = current_state->write_slice_index;
    node->progress.output_slice_num    = current_state->output_slice_num;
    node->progress.output_line_count   = current_state->output_line_count;
    
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
    pthread_cond_signal(&g_worker.cond);
    pthread_mutex_unlock(&g_worker.mutex);
    // 给一点时间让 worker 退出，或者使用 join (如果改用 joinable thread)
    // 这里简单 sleep 等待
    sleep(1); 
}
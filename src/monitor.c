#include "monitor.h"
#include "utils.h"
#include "traversal.h"    // 需包含 traversal_spawn_replacement_worker
#include "async_worker.h" // 需包含 async_worker_get_queue_size
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <math.h>

#define HEARTBEAT_TIMEOUT_SEC 30   // Worker 30秒无心跳视为卡死
#define PROBE_TIMEOUT_SEC 5        // 探针5秒不返回视为设备死亡
#define MONITOR_INTERVAL_MS 500    // Monitor 线程主频 (500ms)
#define CHECK_INTERVAL_SEC 1       // 巡检频率 (1秒)

static void* probe_thread_func(void *arg) {
    ProbeArgs *p = (ProbeArgs*)arg;
    
    struct stat st;
    // 使用 lstat 测试访问性
    // 如果 NFS 挂死，lstat 会卡住，线程无法返回 -> DeviceManager 状态不会更新 -> 触发 Monitor 熔断
    if (lstat(p->path, &st) == 0) {
        dev_mgr_mark_alive(p->mgr, p->dev); // 活过来了/没死
    } else {
        // 即使报错 (如 Permission denied)，只要没卡住，就说明设备是活的
        dev_mgr_mark_alive(p->mgr, p->dev);
    }
    
    free(p);
    return NULL;
}

static void launch_probe(DeviceManager *mgr, dev_t dev, const char *path) {
    if (!mgr) return;

    // 标记为 PROBING，防止重复发射
    dev_mgr_mark_probing(mgr, dev);
    
    ProbeArgs *args = safe_malloc(sizeof(ProbeArgs));
    args->mgr = mgr;
    args->dev = dev;
    strncpy(args->path, path, sizeof(args->path) - 1);
    
    pthread_t tid;
    // 创建 detached 线程，因为它可能永远回不来
    if (pthread_create(&tid, NULL, probe_thread_func, args) == 0) {
        pthread_detach(tid); 
    } else {
        free(args);
    }
}
// ==========================================
// 2. [模块] 统计与展示 (Statistics & Display)
// ==========================================

// 辅助：计算入队/处理速率
double calculate_rate_simple(time_t start_time, unsigned long count) {
    time_t current_time = time(NULL);
    double elapsed = difftime(current_time, start_time);
    if (elapsed < 1.0) return 0.0;
    return (double)count / elapsed;
}

// 更新统计数据 (Ring Buffer 算法)
static void update_statistics(RuntimeState *state) {
    Statistics *st = &state->stats;
    time_t now = time(NULL);
    
    // 限制采样频率：每秒只采一次
    if (now - st->last_sample_time < 1) return;
    
    // 存入新样本
    st->head_idx = (st->head_idx + 1) % RATE_WINDOW_SIZE;
    if (st->head_idx == 0 && st->samples[0].timestamp != 0) {
        st->filled = true;
    }
    
    RateSample *new_s = &st->samples[st->head_idx];
    new_s->timestamp = now;
    new_s->dir_count = state->dir_count;
    new_s->file_count = state->file_count;
    new_s->dequeued_count = state->total_dequeued_count;

    st->last_sample_time = now;

    // 计算窗口差值
    int tail_idx = st->filled ? (st->head_idx + 1) % RATE_WINDOW_SIZE : 0;
    RateSample *old_s = &st->samples[tail_idx];
    double time_diff = difftime(new_s->timestamp, old_s->timestamp);
    
    if (time_diff >= 1.0) {
        // 目录速率
        st->current_dir_rate = (double)(new_s->dir_count - old_s->dir_count) / time_diff;
        if (st->current_dir_rate > st->max_dir_rate) st->max_dir_rate = st->current_dir_rate;

        // 文件速率
        st->current_file_rate = (double)(new_s->file_count - old_s->file_count) / time_diff;
        if (st->current_file_rate > st->max_file_rate) st->max_file_rate = st->current_file_rate;

        // 消费速率
        st->current_dequeue_rate = (double)(new_s->dequeued_count - old_s->dequeued_count) / time_diff;
        if (st->current_dequeue_rate > st->max_dequeue_rate) st->max_dequeue_rate = st->current_dequeue_rate;
    } else {
        // 刚启动不足1秒，使用全局平均兜底
        st->current_dir_rate = calculate_rate_simple(state->start_time, state->dir_count);
        st->current_file_rate = calculate_rate_simple(state->start_time, state->file_count);
    }
}

static void format_elapsed_time(time_t start_time, char *buffer, size_t buf_size) {
    long elapsed = difftime(time(NULL), start_time);
    int days = elapsed / 86400; elapsed %= 86400;
    int hours = elapsed / 3600; elapsed %= 3600;
    int minutes = elapsed / 60;
    int seconds = elapsed % 60;
    snprintf(buffer, buf_size, "%d:%02d:%02d:%02d", days, hours, minutes, seconds);
}

void *status_thread_func(void *arg) {
    ThreadSharedState *shared = (ThreadSharedState *)arg;
    const useconds_t update_interval = 500000; // 500ms

    while (shared->running) {
        display_status(shared);
        usleep(update_interval);
    }
    return NULL;
}

// 打印仪表盘
void print_progress(const Config *cfg, RuntimeState *state, Monitor *mon) {
    // 1. 判断是否需要显示
    // 如果没有重定向输出文件，且没有mute，则不打印(避免污染 stdout)
    bool output_to_stdout = (!cfg->is_output_file && !cfg->is_output_split_dir);
    if (output_to_stdout && !cfg->mute) return;

    // 2. 更新统计 (Hack: 移除 const 以更新统计数据)
    update_statistics((RuntimeState*)state);

    // 3. 确定输出目标
    FILE *target = stdout;
    bool use_ansi = true;

    if (cfg->mute) {
        // Mute 模式：输出到 .status 文件
        if (!state->status_file_fp) {
            char status_path[1024];
            snprintf(status_path, sizeof(status_path), "%s.status", cfg->progress_base);
            ((RuntimeState*)state)->status_file_fp = fopen(status_path, "w");
            if (!state->status_file_fp) ((RuntimeState*)state)->status_file_fp = stdout;
        }
        target = state->status_file_fp;
        if (target != stdout && target != stderr) {
            use_ansi = false;
            rewind(target);
            ftruncate(fileno(target), 0);
        }
    }

    // 4. 绘制
    if (use_ansi) fprintf(target, "\033[0;0H\033[J"); // 清屏
    
    char time_str[32];
    format_elapsed_time(state->start_time, time_str, sizeof(time_str));

    fprintf(target, "===== 异步流水线状态 (v%s) =====\n", VERSION);
    fprintf(target, "运行时间: %s\n", time_str);
    
    // Looper 状态
    fprintf(target, "\n[调度器 (Looper)]\n");
    fprintf(target, "├── 发现速率: %8.2f/s (Max: %.2f)\n", state->stats.current_dir_rate, state->stats.max_dir_rate);
    fprintf(target, "└── 消费速率: %8.2f/s (Max: %.2f)\n", state->stats.current_dequeue_rate, state->stats.max_dequeue_rate);
    
    // Worker 状态
    fprintf(target, "\n[执行器 (Workers: %d/%d)]\n", mon->active_worker_count, mon->worker_capacity);

    // Writer 状态
    // 这里需要 ThreadSharedState 里的 worker 指针才能获取 queue size，暂时略过或通过 state 传递
    // 简单起见只打印速率
    fprintf(target, "└── 落盘速率: %8.2f/s (Max: %.2f)\n", state->stats.current_file_rate, state->stats.max_file_rate);

    // 总体
    fprintf(target, "\n[总体产出]: %lu 文件\n", state->file_count);
    if (cfg->is_output_split_dir) {
        fprintf(target, "当前分片: %lu (行: %lu)\n", state->output_slice_num, state->output_line_count);
    }

    fflush(target);
}

// ==========================================
// 3. [模块] 守护进程巡检 (Daemon Health Check)
// ==========================================

static void check_workers_health(Monitor *self) {
    time_t now = time(NULL);
    DeviceManager *dm = self->state->dev_mgr;
    if (!dm) return;

    pthread_mutex_lock(&self->mutex);
    
    for (int i = 0; i < self->worker_capacity; i++) {
        WorkerHeartbeat *hb = self->workers[i];
        if (!hb) continue;
        
        // 1. 检查是否超时
        if (now - hb->last_active > HEARTBEAT_TIMEOUT_SEC) {
            dev_t dev = hb->current_dev;
            if (dev == 0) continue; 
            
            DeviceState ds = dev_mgr_get_state(dm, dev);
            
            if (ds == DEV_STATE_NORMAL) {
                // -> 首次发现卡顿，发射探针
                if (self->cfg->verbose) {
                    fprintf(stderr, "[Monitor] Worker %d 超时 (dev: %lu), 启动探针...\n", hb->id, (unsigned long)dev);
                }
                launch_probe(dm, dev, hb->current_path);
                
            } else if (ds == DEV_STATE_PROBING) {
                // -> 正在探测，检查是否探测超时 (判定探针死亡)
                // 如果 Worker 持续卡顿且超过 (超时时间 + 探针等待时间)，强制判死
                if (now - hb->last_active > HEARTBEAT_TIMEOUT_SEC + PROBE_TIMEOUT_SEC + 2) {
                    fprintf(stderr, "[Monitor] 探针未返回，确认设备 %lu 死亡！熔断生效。\n", (unsigned long)dev);
                    dev_mgr_mark_dead(dm, dev);
                }
                
            } else if (ds == DEV_STATE_DEAD) {
                // -> 设备已死，清理僵尸
                if (!hb->is_zombie) {
                    hb->is_zombie = true; // 标记自杀
                    fprintf(stderr, "[Monitor] 放弃 Worker %d，补充新线程。\n", hb->id);
                    
                    // 从名册移除 (Slot 置空)，但内存不释放 (由 Worker 线程自杀前释放或 Leak)
                    self->workers[i] = NULL; 
                    self->active_worker_count--;
                    
                    // 标记全局错误，阻止 Success 写入
                    self->state->has_error = true; 
                    
                    // 补位：调用 Traversal 层的接口
                    traversal_spawn_replacement_worker(self->cfg, self);
                }
            }
        }
    }
    
    pthread_mutex_unlock(&self->mutex);
}

// ==========================================
// 4. [模块] 生命周期与主循环 (Lifecycle & Main Loop)
// ==========================================

Monitor* monitor_create(const Config *cfg, RuntimeState *state) {
    Monitor *self = safe_malloc(sizeof(Monitor));
    memset(self, 0, sizeof(Monitor));
    
    self->cfg = cfg;
    self->state = state;
    self->worker_capacity = 256;
    self->workers = safe_malloc(sizeof(WorkerHeartbeat*) * self->worker_capacity);
    self->active_worker_count = 0;
    self->running = true;
    
    pthread_mutex_init(&self->mutex, NULL);
    
    // 注意：dev_mgr 应该在 main.c 中已经创建并赋值给 state->dev_mgr
    // 这里不再创建，而是直接使用
    
    return self;
}

void monitor_destroy(Monitor *self) {
    if (!self) return;
    self->running = false;
    
    pthread_mutex_destroy(&self->mutex);
    
    // 清理所有 Heartbeat
    for (int i = 0; i < self->worker_capacity; i++) {
        if (self->workers[i]) free(self->workers[i]);
    }
    free(self->workers);
    free(self);
}

WorkerHeartbeat* monitor_register_worker(Monitor *self, pthread_t tid) {
    if (!self) return NULL;
    pthread_mutex_lock(&self->mutex);
    
    WorkerHeartbeat *hb = safe_malloc(sizeof(WorkerHeartbeat));
    hb->id = self->active_worker_count + 1; // 仅作逻辑展示ID
    hb->tid = tid;
    hb->last_active = time(NULL);
    hb->current_dev = 0;
    hb->is_zombie = false;
    memset(hb->current_path, 0, sizeof(hb->current_path));
    
    int slot = -1;
    for (int i = 0; i < self->worker_capacity; i++) {
        if (self->workers[i] == NULL) {
            slot = i;
            break;
        }
    }
    
    if (slot != -1) {
        self->workers[slot] = hb;
        self->active_worker_count++;
    } else {
        free(hb);
        hb = NULL;
    }
    
    pthread_mutex_unlock(&self->mutex);
    return hb;
}

void monitor_unregister_worker(Monitor *self, WorkerHeartbeat *hb) {
    if (!self || !hb) return;
    pthread_mutex_lock(&self->mutex);
    for (int i = 0; i < self->worker_capacity; i++) {
        if (self->workers[i] == hb) {
            self->workers[i] = NULL;
            self->active_worker_count--;
            break;
        }
    }
    pthread_mutex_unlock(&self->mutex);
    free(hb);
}

// Monitor 线程入口
void* monitor_thread_entry(void *arg) {
    Monitor *self = (Monitor*)arg;
    time_t last_check_time = 0;

    while (self->running) {
        // 1. 打印进度 (UI 刷新)
        print_progress(self->cfg, self->state, self);
        
        // 2. 守护进程巡检 (Daemon Check)
        time_t now = time(NULL);
        if (now - last_check_time >= CHECK_INTERVAL_SEC) {
            check_workers_health(self);
            last_check_time = now;
        }
        
        // 3. 休眠
        usleep(MONITOR_INTERVAL_MS * 1000);
    }
    return NULL;
}

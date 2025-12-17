#include "monitor.h"
#include "utils.h"
#include "async_worker.h" 
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <time.h>
#include <math.h>


// 入队效率
double calculate_rate(time_t start_time, unsigned long count) {
    time_t current_time = time(NULL);
    double elapsed = difftime(current_time, start_time);
    if (elapsed < 1.0) return 0.0;  // 避免除以零
    return (double)count / elapsed;
}


// === 新增：更新统计数据 ===
// 注意：此函数修改了 state，因此需要转换 const 指针
static void update_statistics(const RuntimeState *c_state) {
    RuntimeState *state = (RuntimeState *)c_state; // 移除 const
    Statistics *st = &state->stats;
    
    time_t now = time(NULL);
    
    // 限制采样频率：每秒只采一次，避免计算过于抖动
    if (now - st->last_sample_time < 1) {
        return;
    }
    
    // 1. 存入新样本 (Ring Buffer)
    st->head_idx = (st->head_idx + 1) % RATE_WINDOW_SIZE;
    if (st->head_idx == 0 && st->samples[0].timestamp != 0) {
        st->filled = true;
    }
    
    st->samples[st->head_idx].timestamp = now;
    st->samples[st->head_idx].dir_count = state->dir_count;
    st->samples[st->head_idx].file_count = state->file_count;
    st->samples[st->head_idx].dequeued_count = state->total_dequeued_count; // <--- 记录出队数
    st->last_sample_time = now;
    int tail_idx = st->filled ? (st->head_idx + 1) % RATE_WINDOW_SIZE : 0;
    RateSample *new_s = &st->samples[st->head_idx];
    RateSample *old_s = &st->samples[tail_idx];
    double time_diff = difftime(new_s->timestamp, old_s->timestamp);
    
   
    if (time_diff >= 1.0) {
        // 目录速率
        unsigned long dir_diff = new_s->dir_count - old_s->dir_count;
        st->current_dir_rate = (double)dir_diff / time_diff;
        if (st->current_dir_rate > st->max_dir_rate) st->max_dir_rate = st->current_dir_rate;

        // 文件速率
        unsigned long file_diff = new_s->file_count - old_s->file_count;
        st->current_file_rate = (double)file_diff / time_diff;
        if (st->current_file_rate > st->max_file_rate) st->max_file_rate = st->current_file_rate;

        unsigned long deq_diff = new_s->dequeued_count - old_s->dequeued_count;
        st->current_dequeue_rate = (double)deq_diff / time_diff;
        if (st->current_dequeue_rate > st->max_dequeue_rate) 
            st->max_dequeue_rate = st->current_dequeue_rate;
    } else {
        // 刚启动不足1秒，使用全局平均兜底
        // calculate_rate 是原来的全局算法，可以保留作为 fallback
        st->current_dir_rate = calculate_rate(state->start_time, state->dir_count);
        st->current_file_rate = calculate_rate(state->start_time, state->file_count);
    }
}

// 计算已执行时间并格式化
static void format_elapsed_time(time_t start_time, char *buffer, size_t buf_size) {
    time_t current_time = time(NULL);
    long elapsed = difftime(current_time, start_time);
    
    int days = elapsed / 86400;
    elapsed %= 86400;
    int hours = elapsed / 3600;
    elapsed %= 3600;
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

// === 清理后的 display_status ===
void display_status(const ThreadSharedState *shared) {
    const Config *cfg = shared->cfg;
    const RuntimeState *state = shared->state; 
    
    // 【Hack 说明】: display_status 本质上是一个只读展示函数，但我们需要在这里驱动
    // 统计数据的更新（如计算每秒速率）。因此这里进行了一次去 const 转换。
    RuntimeState *mutable_state = (RuntimeState *)state;
    
    // 1. 判断是否需要显示
    // 逻辑：如果结果输出到屏幕(stdout)，则不能打印仪表盘（会混淆数据）。
    // 除非：用户开启了 --mute，此时仪表盘会重定向到 .status 文件。
    bool output_to_stdout = (!cfg->is_output_file && !cfg->is_output_split_dir);
    if (output_to_stdout && !cfg->mute) {
        return; 
    }

    // 2. 更新速率统计 (Ring Buffer 计算)
    update_statistics(mutable_state);

    // 3. 获取异步写入队列的积压量 (这是唯一剩下的显式队列指标)
    size_t async_pending = async_worker_get_queue_size(shared->worker);

    // 4. 确定输出目标
    FILE *target = stdout; // 默认屏幕
    bool use_ansi = true;  // 默认使用动画

    if (cfg->mute) {
        // --- Mute 模式：输出到 .status 文件 ---
        // 懒加载：第一次运行时打开文件
        if (!mutable_state->status_file_fp) {
            char status_path[1024];
            snprintf(status_path, sizeof(status_path), "%s.status", cfg->progress_base);
            mutable_state->status_file_fp = fopen(status_path, "w");
            if (!mutable_state->status_file_fp) {
                mutable_state->status_file_fp = stdout; // 失败降级
            }
        }
        target = mutable_state->status_file_fp;
        
        // 如果是文件，禁用 ANSI 颜色，并执行“原地刷新”魔法
        if (target != stdout && target != stderr) {
            use_ansi = false;
            rewind(target);               // 指针回到文件头
            ftruncate(fileno(target), 0); // 截断文件内容
        }
    }

    // 5. 绘制仪表盘
    if (use_ansi) fprintf(target, "\033[0;0H\033[J"); // ANSI 清屏 + 光标复位
    
    fprintf(target, "===== 异步流水线状态 (v%s) =====\n", VERSION);
    
    char time_str[32];
    format_elapsed_time(state->start_time, time_str, sizeof(time_str));
    fprintf(target, "运行时间: %s\n", time_str);

    fprintf(target, "\n[Looper 调度器]\n");
    // Looper 架构下，我们不再直接显示队列深度，而是通过速率差来判断压力
    // 发现速率 > 消费速率 = 积压；发现速率 < 消费速率 = 消化
    fprintf(target, "├── 发现速率: %.2f 个/秒 (峰值: %.2f)\n", 
           state->stats.current_dir_rate, state->stats.max_dir_rate);
    fprintf(target, "└── 消费速率: %.2f 个/秒 (峰值: %.2f)\n", 
           state->stats.current_dequeue_rate, state->stats.max_dequeue_rate);

    double trend = state->stats.current_dir_rate - state->stats.current_dequeue_rate;
    fprintf(target, "    └── 负载趋势: %s%.2f/s %s\n", 
            trend > 0 ? "+" : "", trend, 
            trend > 0 ? "(积压中)" : "(消化中)");

    fprintf(target, "\n[AsyncWorker 写入]\n");
    fprintf(target, "├── 写入缓冲: %zu (待落盘)\n", async_pending);
    fprintf(target, "└── 落盘速率: %.2f 个/秒 (峰值: %.2f)\n", 
           state->stats.current_file_rate, state->stats.max_file_rate);

    fprintf(target, "\n[总体进度]\n");
    if (cfg->is_output_split_dir) {
        fprintf(target, "当前分片: " OUTPUT_SLICE_FORMAT " (行数: %lu / %lu)\n", 
               state->output_slice_num, state->output_line_count, cfg->output_slice_lines);
    }
    fprintf(target, "总产出量: %lu 文件\n", state->file_count);

    if (cfg->continue_mode) {
        fprintf(target, "断点保护: 分片 " PROGRESS_SLICE_FORMAT " (Offset: %lu)\n", 
               state->write_slice_index, state->line_count % DEFAULT_PROGRESS_SLICE_LINES);
    }
    
    // 显示当前正在处理的路径 (如果有)
    // 注意：在 Looper 模式下，state->current_path 可能更新不及时，仅供参考
    if (state->current_path) {
        fprintf(target, "\n当前扫描: %s\n", state->current_path);
    }
    
    fflush(target);
}
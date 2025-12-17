#ifndef MONITOR_H
#define MONITOR_H

#include "config.h"

double calculate_rate(time_t start_time, unsigned long count);

// 监控线程的主循环函数
void *status_thread_func(void *arg);

// 单次显示状态（如果需要手动调用）
void display_status(const ThreadSharedState *shared);

#endif // MONITOR_H

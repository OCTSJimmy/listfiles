#ifndef MONITOR_H
#define MONITOR_H

#include "config.h"
#include "device_manager.h"

// 前置声明
struct WorkerHeartbeat;
struct DeviceManager;

// Monitor 上下文结构体
typedef struct Monitor {
    const Config *cfg;
    RuntimeState *state;
    
    // Worker 管理
    struct WorkerHeartbeat **workers;
    int worker_capacity;
    int active_worker_count;
    pthread_mutex_t mutex;
    
    // 运行状态
    bool running;
    pthread_t monitor_tid;
} Monitor;

// Worker 心跳包
typedef struct WorkerHeartbeat {
    int id;                 // 逻辑 ID
    pthread_t tid;          // 线程 ID
    _Atomic time_t last_active; // 最后活跃时间
    _Atomic dev_t current_dev;  // 当前所在的设备
    _Atomic bool is_zombie;     // 是否被标记为僵尸 (需要自杀)
    char current_path[1024];    // 当前路径 (用于探针)
} WorkerHeartbeat;

// ==========================================
// 1. [模块] 敢死队探针 (Probe Logic)
// ==========================================

typedef struct {
    DeviceManager *mgr; // [新增] 需要持有管理器指针来更新状态
    dev_t dev;
    char path[1024];
} ProbeArgs;

// 创建并初始化 Monitor
Monitor* monitor_create(const Config *cfg, RuntimeState *state);

// 销毁 Monitor
void monitor_destroy(Monitor *self);

// 注册 Worker (Worker 线程启动时调用)
WorkerHeartbeat* monitor_register_worker(Monitor *self, pthread_t tid);

// 注销 Worker (Worker 线程退出时调用)
void monitor_unregister_worker(Monitor *self, WorkerHeartbeat *hb);

// 启动监控循环 (通常在单独线程跑)
void* monitor_thread_entry(void *arg);

double calculate_rate(time_t start_time, unsigned long count);

// 监控线程的主循环函数
void *status_thread_func(void *arg);

// 单次显示状态（如果需要手动调用）
void display_status(const ThreadSharedState *shared);

#endif // MONITOR_H

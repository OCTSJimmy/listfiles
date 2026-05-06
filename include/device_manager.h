#ifndef DEVICE_MANAGER_H
#define DEVICE_MANAGER_H

#include <sys/types.h>
#include <stdbool.h>
#include <time.h>
#include <pthread.h>
#include <stdatomic.h>

#define MAX_TRACKED_DEVICES 1024

// 设备状态枚举
typedef enum {
    DEV_STATE_NORMAL = 0, // 正常
    DEV_STATE_PROBING,    // 正在探测中 (嫌疑)
    DEV_STATE_DEAD,       // 已熔断 (黑名单)
    DEV_STATE_CONDEMNED   // 已判死 (永久跳过，不探测)
} DeviceState;

// 内部条目 (对外部不透明，但定义在头文件方便内联或查看)
typedef struct {
    dev_t dev;
    _Atomic uint32_t state;  /* DeviceState 的底层值，原子读写 */
    time_t last_probe_time;
} DeviceEntry;

// 设备管理器上下文结构体
typedef struct DeviceManager {
    DeviceEntry entries[MAX_TRACKED_DEVICES];
    _Atomic size_t count;
    pthread_mutex_t mutex;   /* 仅保护写路径（添加/修改条目） */
} DeviceManager;

// === 接口 ===

// 初始化 (分配内存并初始化锁)
DeviceManager* dev_mgr_create();

// 销毁
void dev_mgr_destroy(DeviceManager *self);

// 获取设备状态 (无锁读路径，高并发安全)
DeviceState dev_mgr_get_state(DeviceManager *self, dev_t dev);

// 状态标记 (mutex 保护写路径)
void dev_mgr_mark_probing(DeviceManager *self, dev_t dev);
void dev_mgr_mark_dead(DeviceManager *self, dev_t dev);
void dev_mgr_mark_alive(DeviceManager *self, dev_t dev);
void dev_mgr_mark_condemned(DeviceManager *self, dev_t dev);

// 快捷查询 (无锁)
bool dev_mgr_is_blacklisted(DeviceManager *self, dev_t dev);

#endif // DEVICE_MANAGER_H

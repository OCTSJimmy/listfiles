/**
 * @file device_manager.c
 * @brief 设备状态机管理器实现
 *
 * 维护一个固定大小的设备状态数组（MAX_TRACKED_DEVICES=1024），
 * 通过原子读 + mutex 写的方式实现高并发安全。
 * 设备状态流转: NORMAL → PROBING → DEAD → CONDEMNED
 * 用于在 Worker 上报设备级错误（ETIMEDOUT/EIO）时自动熔断对应设备，
 * 避免整个扫描进程因单个存储设备无响应而陷入 D-State 阻塞。
 */
#include "device_manager.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "log.h"

/**
 * @brief  设备管理器内部的安全内存分配函数
 * @param  size  size_t  要分配的内存字节数，取值范围: > 0
 * @return void*  指向成功分配内存的指针，不会返回 NULL
 *
 * @note   若 malloc 失败，向 stderr 输出错误并调用 exit(1) 终止程序。
 *         与全局 safe_malloc 行为一致，避免 device_manager 模块对外部 utils 产生循环依赖。
 */
static void *dm_safe_malloc(size_t size) {
    void *ptr = malloc(size);
    if (!ptr) {
        log_fatal("Out of memory in device_manager");
        exit(1);
    }
    return ptr;
}

/**
 * @brief  创建 DeviceManager 实例
 * @return DeviceManager*  成功返回指向新分配管理器的指针；内存不足时退出进程
 *
 * @note   内部数组通过 memset 清零，count 原子初始化为 0，mutex 初始化为默认属性。
 *         设备条目(entries)采用静态数组，避免运行时的动态扩容开销。
 */
DeviceManager* dev_mgr_create() {
    DeviceManager *self = dm_safe_malloc(sizeof(DeviceManager));
    memset(self, 0, sizeof(DeviceManager));
    atomic_init(&self->count, 0);
    pthread_mutex_init(&self->mutex, NULL);
    return self;
}

/**
 * @brief  销毁 DeviceManager 实例
 * @param  self  DeviceManager*  要销毁的管理器指针，允许传入 NULL（空操作）
 * @return void
 */
void dev_mgr_destroy(DeviceManager *self) {
    if (!self) return;
    pthread_mutex_destroy(&self->mutex);
    free(self);
}

/**
 * @brief  无锁查询指定设备的当前状态
 * @param  self  DeviceManager*  设备管理器指针，允许传入 NULL（此时返回 DEV_STATE_NORMAL）
 * @param  dev   dev_t            要查询的设备号，取值范围: 有效 Linux 设备号
 * @return DeviceState  设备当前状态，取值范围: {DEV_STATE_NORMAL, DEV_STATE_PROBING, DEV_STATE_DEAD, DEV_STATE_CONDEMNED}
 *
 * @note   本函数为无锁读路径，直接遍历原子变量 count 范围内的条目数组。
 *         若设备未在追踪列表中，返回 DEV_STATE_NORMAL。
 *         由于 count 为原子变量，读操作可与写操作并发执行。
 */
DeviceState dev_mgr_get_state(DeviceManager *self, dev_t dev) {
    if (!self) return DEV_STATE_NORMAL;
    size_t n = atomic_load(&self->count);
    for (size_t i = 0; i < n; i++) {
        if (self->entries[i].dev == dev) {
            return (DeviceState)atomic_load(&self->entries[i].state);
        }
    }
    return DEV_STATE_NORMAL;
}

/**
 * @brief  mutex 保护的内部查找函数（写路径专用）
 * @param  self  DeviceManager*  设备管理器指针，不能为空；调用方必须已持有 self->mutex
 * @param  dev   dev_t            要查找的设备号
 * @return int   找到时返回条目索引（>= 0）；未找到时返回 -1
 *
 * @note   本函数不自行加锁，仅应在 mutex 保护区域内调用。
 */
static int find_index_locked(DeviceManager *self, dev_t dev) {
    size_t n = atomic_load(&self->count);
    for (size_t i = 0; i < n; i++) {
        if (self->entries[i].dev == dev) {
            return (int)i;
        }
    }
    return -1;
}

/**
 * @brief  mutex 保护的内部状态更新函数
 * @param  self       DeviceManager*  设备管理器指针，不能为空；调用方必须已持有 self->mutex
 * @param  dev        dev_t            要更新的设备号
 * @param  new_state  DeviceState      新的设备状态，取值范围: 任意 DeviceState 枚举值
 * @return void
 *
 * @note   若设备已存在，则原子更新其状态；若为 PROBING 状态同时记录当前时间到 last_probe_time。
 *         若设备不存在且追踪列表未满，则追加新条目。
 *         若追踪列表已满（>= MAX_TRACKED_DEVICES），仅打印一次警告后静默丢弃。
 */
static void update_state_locked(DeviceManager *self, dev_t dev, DeviceState new_state) {
    int idx = find_index_locked(self, dev);
    if (idx != -1) {
        atomic_store(&self->entries[idx].state, (uint32_t)new_state);
        if (new_state == DEV_STATE_PROBING) {
            self->entries[idx].last_probe_time = time(NULL);
        }
    } else {
        size_t n = atomic_load(&self->count);
        if (n < MAX_TRACKED_DEVICES) {
            DeviceEntry *entry = &self->entries[n];
            entry->dev = dev;
            atomic_store(&entry->state, (uint32_t)new_state);
            entry->last_probe_time = (new_state == DEV_STATE_PROBING) ? time(NULL) : 0;
            atomic_store(&self->count, n + 1);
        } else {
            static bool warned = false;
            if (!warned) {
                log_warn("Device Manager full! Cannot track dev %lu", (unsigned long)dev);
                warned = true;
            }
        }
    }
}

/**
 * @brief  将指定设备标记为 PROBING（探测中）状态
 * @param  self  DeviceManager*  设备管理器指针，允许传入 NULL（空操作）
 * @param  dev   dev_t            目标设备号
 * @return void
 *
 * @note   若设备当前已为 DEAD 状态，则保持 DEAD 不降级为 PROBING。
 *         本操作为写路径，内部自动加锁。
 */
void dev_mgr_mark_probing(DeviceManager *self, dev_t dev) {
    if (!self) return;
    pthread_mutex_lock(&self->mutex);
    int idx = find_index_locked(self, dev);
    if (idx == -1 || (DeviceState)atomic_load(&self->entries[idx].state) != DEV_STATE_DEAD) {
        update_state_locked(self, dev, DEV_STATE_PROBING);
    }
    pthread_mutex_unlock(&self->mutex);
}

/**
 * @brief  将指定设备标记为 DEAD（已熔断）状态
 * @param  self  DeviceManager*  设备管理器指针，允许传入 NULL（空操作）
 * @param  dev   dev_t            目标设备号
 * @return void
 *
 * @note   设备进入 DEAD 状态后，所有属于该设备的扫描任务将被跳过，
 *         同时向 ProbeScheduler 注册探测任务以尝试后续恢复。
 */
void dev_mgr_mark_dead(DeviceManager *self, dev_t dev) {
    if (!self) return;
    pthread_mutex_lock(&self->mutex);
    update_state_locked(self, dev, DEV_STATE_DEAD);
    pthread_mutex_unlock(&self->mutex);
}

/**
 * @brief  将指定设备标记为 NORMAL（正常）状态
 * @param  self  DeviceManager*  设备管理器指针，允许传入 NULL（空操作）
 * @param  dev   dev_t            目标设备号
 * @return void
 *
 * @note   通常在探测成功（敢死队 lstat 返回）后调用，使设备恢复可用。
 */
void dev_mgr_mark_alive(DeviceManager *self, dev_t dev) {
    if (!self) return;
    pthread_mutex_lock(&self->mutex);
    update_state_locked(self, dev, DEV_STATE_NORMAL);
    pthread_mutex_unlock(&self->mutex);
}

/**
 * @brief  将指定设备标记为 CONDEMNED（已判死）状态
 * @param  self  DeviceManager*  设备管理器指针，允许传入 NULL（空操作）
 * @param  dev   dev_t            目标设备号
 * @return void
 *
 * @note   CONDEMNED 为终态，设备被标记后将永久跳过，不再进行任何探测。
 *         通常在探测退避达到上限（PROBE_INTERVAL_MAX）后触发。
 */
void dev_mgr_mark_condemned(DeviceManager *self, dev_t dev) {
    if (!self) return;
    pthread_mutex_lock(&self->mutex);
    update_state_locked(self, dev, DEV_STATE_CONDEMNED);
    pthread_mutex_unlock(&self->mutex);
}

/**
 * @brief  判断指定设备是否在黑名单中（DEAD 或 CONDEMNED）
 * @param  self  DeviceManager*  设备管理器指针，允许传入 NULL（此时返回 false）
 * @param  dev   dev_t            目标设备号
 * @return bool  返回 true 表示设备已被熔断或判死，应跳过扫描；false 表示设备正常
 *
 * @note   本函数为无锁读路径，线程安全。
 */
bool dev_mgr_is_blacklisted(DeviceManager *self, dev_t dev) {
    DeviceState s = dev_mgr_get_state(self, dev);
    return s == DEV_STATE_DEAD || s == DEV_STATE_CONDEMNED;
}

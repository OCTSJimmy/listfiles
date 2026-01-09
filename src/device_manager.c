#include "device_manager.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

// 简单的安全内存分配
static void *dm_safe_malloc(size_t size) {
    void *ptr = malloc(size);
    if (!ptr) {
        fprintf(stderr, "[Fatal] Out of memory in device_manager\n");
        exit(1);
    }
    return ptr;
}

DeviceManager* dev_mgr_create() {
    DeviceManager *self = dm_safe_malloc(sizeof(DeviceManager));
    memset(self, 0, sizeof(DeviceManager));
    self->count = 0;
    pthread_mutex_init(&self->mutex, NULL);
    return self;
}

void dev_mgr_destroy(DeviceManager *self) {
    if (!self) return;
    pthread_mutex_destroy(&self->mutex);
    free(self);
}

// 内部查找 (需在锁内调用)
static int find_index_locked(DeviceManager *self, dev_t dev) {
    for (size_t i = 0; i < self->count; i++) {
        if (self->entries[i].dev == dev) {
            return (int)i;
        }
    }
    return -1;
}

DeviceState dev_mgr_get_state(DeviceManager *self, dev_t dev) {
    if (!self) return DEV_STATE_NORMAL;
    
    DeviceState state = DEV_STATE_NORMAL;
    pthread_mutex_lock(&self->mutex);
    int idx = find_index_locked(self, dev);
    if (idx != -1) {
        state = self->entries[idx].state;
    }
    pthread_mutex_unlock(&self->mutex);
    return state;
}

static void update_state_locked(DeviceManager *self, dev_t dev, DeviceState new_state) {
    int idx = find_index_locked(self, dev);
    
    if (idx != -1) {
        // 更新现有
        self->entries[idx].state = new_state;
        if (new_state == DEV_STATE_PROBING) {
            self->entries[idx].last_probe_time = time(NULL);
        }
    } else {
        // 新增
        if (self->count < MAX_TRACKED_DEVICES) {
            DeviceEntry *entry = &self->entries[self->count++];
            entry->dev = dev;
            entry->state = new_state;
            entry->last_probe_time = (new_state == DEV_STATE_PROBING) ? time(NULL) : 0;
        } else {
            // 满了，仅打印一次警告
            static bool warned = false;
            if (!warned) {
                fprintf(stderr, "[Warn] Device Manager full! Cannot track dev %lu\n", (unsigned long)dev);
                warned = true;
            }
        }
    }
}

void dev_mgr_mark_probing(DeviceManager *self, dev_t dev) {
    if (!self) return;
    pthread_mutex_lock(&self->mutex);
    int idx = find_index_locked(self, dev);
    // 只有非 DEAD 状态才允许转 PROBING
    if (idx == -1 || self->entries[idx].state != DEV_STATE_DEAD) {
        update_state_locked(self, dev, DEV_STATE_PROBING);
    }
    pthread_mutex_unlock(&self->mutex);
}

void dev_mgr_mark_dead(DeviceManager *self, dev_t dev) {
    if (!self) return;
    pthread_mutex_lock(&self->mutex);
    update_state_locked(self, dev, DEV_STATE_DEAD);
    pthread_mutex_unlock(&self->mutex);
}

void dev_mgr_mark_alive(DeviceManager *self, dev_t dev) {
    if (!self) return;
    pthread_mutex_lock(&self->mutex);
    update_state_locked(self, dev, DEV_STATE_NORMAL);
    pthread_mutex_unlock(&self->mutex);
}

bool dev_mgr_is_blacklisted(DeviceManager *self, dev_t dev) {
    return dev_mgr_get_state(self, dev) == DEV_STATE_DEAD;
}
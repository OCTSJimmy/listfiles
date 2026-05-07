#include "device_manager.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

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
    atomic_init(&self->count, 0);
    pthread_mutex_init(&self->mutex, NULL);
    return self;
}

void dev_mgr_destroy(DeviceManager *self) {
    if (!self) return;
    pthread_mutex_destroy(&self->mutex);
    free(self);
}

/* 无锁读路径：直接遍历原子状态数组 */
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

/* mutex 保护的内部查找（写路径） */
static int find_index_locked(DeviceManager *self, dev_t dev) {
    size_t n = atomic_load(&self->count);
    for (size_t i = 0; i < n; i++) {
        if (self->entries[i].dev == dev) {
            return (int)i;
        }
    }
    return -1;
}

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
    if (idx == -1 || (DeviceState)atomic_load(&self->entries[idx].state) != DEV_STATE_DEAD) {
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

void dev_mgr_mark_condemned(DeviceManager *self, dev_t dev) {
    if (!self) return;
    pthread_mutex_lock(&self->mutex);
    update_state_locked(self, dev, DEV_STATE_CONDEMNED);
    pthread_mutex_unlock(&self->mutex);
}

bool dev_mgr_is_blacklisted(DeviceManager *self, dev_t dev) {
    DeviceState s = dev_mgr_get_state(self, dev);
    return s == DEV_STATE_DEAD || s == DEV_STATE_CONDEMNED;
}

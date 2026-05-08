/**
 * @file probe_scheduler.c
 * @brief 渐进探测调度器实现
 *
 * 基于小根堆（min-heap）的定时任务调度器，按 next_probe_time 排序。
 * 对熔断设备执行指数退避探测策略：5s → 10s → 20s → ... → 300s。
 * 线程安全封装：所有堆操作均在 pthread_mutex_t 保护下进行。
 */
#include "probe_scheduler.h"
#include "spbin.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ================================================================
 * Min-heap ordered by next_probe_time
 * ================================================================ */

/**
 * @brief  交换两个探测任务的内容
 * @param  a  ProbeTask*  指向第一个任务的指针，不能为空
 * @param  b  ProbeTask*  指向第二个任务的指针，不能为空
 * @return void
 */
static inline void swap_task(ProbeTask *a, ProbeTask *b) {
    ProbeTask t = *a; *a = *b; *b = t;
}

/**
 * @brief  堆上浮操作（将 idx 位置的元素向上调整至正确位置）
 * @param  sched  ProbeScheduler*  探测调度器指针，不能为空；调用方必须已持有 sched->mutex
 * @param  idx    size_t            要调整的元素索引，取值范围: [0, sched->count-1]
 * @return void
 *
 * @note   比较父节点与子节点的 next_probe_time，确保父节点值 <= 子节点值。
 *         时间复杂度 O(log n)。
 */
static void heap_up(ProbeScheduler *sched, size_t idx) {
    while (idx > 0) {
        size_t parent = (idx - 1) >> 1;
        if (sched->tasks[parent].next_probe_time <= sched->tasks[idx].next_probe_time)
            break;
        swap_task(&sched->tasks[parent], &sched->tasks[idx]);
        idx = parent;
    }
}

/**
 * @brief  堆下沉操作（将 idx 位置的元素向下调整至正确位置）
 * @param  sched  ProbeScheduler*  探测调度器指针，不能为空；调用方必须已持有 sched->mutex
 * @param  idx    size_t            要调整的元素索引，取值范围: [0, sched->count-1]
 * @return void
 *
 * @note   比较当前节点与左右子节点的 next_probe_time，与最小者交换。
 *         时间复杂度 O(log n)。
 */
static void heap_down(ProbeScheduler *sched, size_t idx) {
    size_t n = sched->count;
    while (1) {
        size_t left = (idx << 1) + 1;
        size_t right = left + 1;
        size_t smallest = idx;

        if (left < n && sched->tasks[left].next_probe_time < sched->tasks[smallest].next_probe_time)
            smallest = left;
        if (right < n && sched->tasks[right].next_probe_time < sched->tasks[smallest].next_probe_time)
            smallest = right;

        if (smallest == idx) break;
        swap_task(&sched->tasks[idx], &sched->tasks[smallest]);
        idx = smallest;
    }
}

/**
 * @brief  创建 ProbeScheduler 实例
 * @return ProbeScheduler*  成功返回指向新分配调度器的指针；内存不足时返回 NULL
 *
 * @note   初始堆容量为 64，count 为 0。堆按 next_probe_time 升序排列。
 */
ProbeScheduler* probe_scheduler_create(void) {
    ProbeScheduler *sched = calloc(1, sizeof(ProbeScheduler));
    if (!sched) return NULL;
    sched->capacity = 64;
    sched->tasks = calloc(sched->capacity, sizeof(ProbeTask));
    if (!sched->tasks) { free(sched); return NULL; }
    sched->count = 0;
    pthread_mutex_init(&sched->mutex, NULL);
    return sched;
}

/**
 * @brief  销毁 ProbeScheduler 实例并释放堆内存
 * @param  sched  ProbeScheduler*  要销毁的调度器指针，允许传入 NULL（空操作）
 * @return void
 */
void probe_scheduler_destroy(ProbeScheduler *sched) {
    if (!sched) return;
    pthread_mutex_destroy(&sched->mutex);
    free(sched->tasks);
    free(sched);
}

/**
 * @brief  向调度器推入一个新的探测任务
 * @param  sched  ProbeScheduler*  目标调度器指针，不能为空
 * @param  task   const ProbeTask*  要推入的任务指针，不能为空
 * @return void
 *
 * @note   若堆已满（count >= capacity），则自动扩容至 2 倍容量。
 *         推入后执行 heap_up 维护堆序。线程安全，内部自动加锁。
 */
void probe_scheduler_push(ProbeScheduler *sched, const ProbeTask *task) {
    pthread_mutex_lock(&sched->mutex);
    if (sched->count >= sched->capacity) {
        size_t new_cap = sched->capacity << 1;
        ProbeTask *new_tasks = realloc(sched->tasks, new_cap * sizeof(ProbeTask));
        if (!new_tasks) { pthread_mutex_unlock(&sched->mutex); return; }
        sched->tasks = new_tasks;
        sched->capacity = new_cap;
    }
    sched->tasks[sched->count] = *task;
    heap_up(sched, sched->count);
    sched->count++;
    pthread_mutex_unlock(&sched->mutex);
}

/**
 * @brief  查看堆顶任务是否已到执行时间（peek，不删除）
 * @param  sched  const ProbeScheduler*  目标调度器指针，不能为空
 * @param  out    ProbeTask*             输出缓冲区，用于拷贝堆顶任务内容；允许为 NULL（仅做存在性判断）
 * @return bool   返回 true 表示存在已到期的任务且内容已拷贝到 out；false 表示无到期任务或堆为空
 *
 * @note   比较堆顶任务的 next_probe_time 与当前系统时间(time(NULL))。
 *         线程安全，内部自动加锁。本函数不会移除堆顶元素。
 */
bool probe_scheduler_peek(const ProbeScheduler *sched, ProbeTask *out) {
    pthread_mutex_lock((pthread_mutex_t*)&sched->mutex);
    if (sched->count == 0) { pthread_mutex_unlock((pthread_mutex_t*)&sched->mutex); return false; }
    time_t now = time(NULL);
    if (sched->tasks[0].next_probe_time > now) { pthread_mutex_unlock((pthread_mutex_t*)&sched->mutex); return false; }
    if (out) *out = sched->tasks[0];
    pthread_mutex_unlock((pthread_mutex_t*)&sched->mutex);
    return true;
}

/**
 * @brief  弹出堆顶元素（内部使用，调用方应已通过 peek 确认存在且已持有 mutex）
 * @param  sched  ProbeScheduler*  目标调度器指针，不能为空；调用方必须已持有 sched->mutex
 * @return void
 *
 * @note   将堆尾元素移至堆顶后执行 heap_down 维护堆序。
 *         本函数为内部辅助函数，不对外暴露，不负责加锁。
 */
static void heap_pop(ProbeScheduler *sched) {
    if (sched->count == 0) return;
    sched->count--;
    if (sched->count > 0) {
        sched->tasks[0] = sched->tasks[sched->count];
        heap_down(sched, 0);
    }
}

/**
 * @brief  从调度器中移除指定设备的所有探测任务
 * @param  sched  ProbeScheduler*  目标调度器指针，不能为空
 * @param  dev    dev_t            要移除的设备号
 * @return void
 *
 * @note   遍历堆数组查找匹配设备，找到后用堆尾元素填充并执行 heap_up/heap_down 恢复堆序。
 *         若不存在该设备的任务，则不做任何操作。线程安全，内部自动加锁。
 */
void probe_scheduler_remove_dev(ProbeScheduler *sched, dev_t dev) {
    pthread_mutex_lock(&sched->mutex);
    for (size_t i = 0; i < sched->count; i++) {
        if (sched->tasks[i].dev == dev) {
            sched->count--;
            if (i < sched->count) {
                sched->tasks[i] = sched->tasks[sched->count];
                heap_up(sched, i);
                heap_down(sched, i);
            }
            pthread_mutex_unlock(&sched->mutex);
            return;
        }
    }
    pthread_mutex_unlock(&sched->mutex);
}

/**
 * @brief  将指定设备的探测任务标记为 CONDEMNED（已判死）
 * @param  sched  ProbeScheduler*  目标调度器指针，不能为空
 * @param  dev    dev_t            目标设备号
 * @return void
 *
 * @note   仅修改任务状态字段，不删除任务条目。
 *         被标记为 CONDEMNED 的任务在后续 peek/dispatch 时会被直接移除。
 *         线程安全，内部自动加锁。
 */
void probe_scheduler_mark_condemned(ProbeScheduler *sched, dev_t dev) {
    pthread_mutex_lock(&sched->mutex);
    for (size_t i = 0; i < sched->count; i++) {
        if (sched->tasks[i].dev == dev) {
            sched->tasks[i].s_status = SP_STATUS_CONDEMNED;
            pthread_mutex_unlock(&sched->mutex);
            return;
        }
    }
    pthread_mutex_unlock(&sched->mutex);
}

/**
 * @brief  探测失败后重新调度堆顶任务（指数退避）
 * @param  sched  ProbeScheduler*  目标调度器指针，不能为空
 * @return void
 *
 * @note   弹出当前到期的堆顶任务，将其 retry_count 加 1，probe_interval 翻倍（上限 PROBE_INTERVAL_MAX=300s），
 *         重新计算 next_probe_time 后推回堆中。
 *         若堆为空或堆顶任务未到期，则不做任何操作。线程安全，内部自动加锁。
 */
void probe_scheduler_reschedule_after_failure(ProbeScheduler *sched) {
    pthread_mutex_lock(&sched->mutex);
    if (sched->count == 0) { pthread_mutex_unlock(&sched->mutex); return; }
    time_t now = time(NULL);
    if (sched->tasks[0].next_probe_time > now) { pthread_mutex_unlock(&sched->mutex); return; }

    ProbeTask task = sched->tasks[0];
    heap_pop(sched);

    task.retry_count++;
    task.probe_interval *= 2;
    if (task.probe_interval > PROBE_INTERVAL_MAX)
        task.probe_interval = PROBE_INTERVAL_MAX;
    task.next_probe_time = now + task.probe_interval;

    sched->tasks[sched->count] = task;
    heap_up(sched, sched->count);
    sched->count++;
    pthread_mutex_unlock(&sched->mutex);
}

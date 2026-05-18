/**
 * @file reference_map.c
 * @brief 指纹 → (mtime, d_type) 映射表实现
 *
 * 基于开放寻址法（线性探测）的哈希表，用于支撑半增量扫描中的 blind-trust 机制。
 * 当文件/目录的 mtime 超过 skip_interval 未变化时，可直接复用历史记录中的元数据，
 * 避免重复的 lstat 系统调用，显著降低 I/O 开销。
 *
 * 本模块与 fingerprint_set.c 使用相同的 splitmix64 哈希函数，确保哈希一致性。
 */
#include "reference_map.h"
#include <stdlib.h>
#include <string.h>

/* 与 fingerprint_set.c 使用完全相同的哈希函数 */

/**
 * @brief  splitmix64 伪随机数生成/哈希混淆函数
 * @param  x  uint64_t  输入种子值，取值范围: 任意 64-bit 无符号整数
 * @return uint64_t  经过多轮位运算混淆后的输出值
 *
 * @note   该算法具有优异的统计特性，可将不均匀输入均匀散布到整个 64-bit 空间。
 *         常用于哈希表的初始哈希值计算。
 */
static inline uint64_t splitmix64(uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x  = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x  = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

/**
 * @brief  计算指纹的哈希桶索引
 * @param  md5       const uint8_t[FP_SIZE]  16 字节指纹数据
 * @param  capacity  size_t                   哈希表当前容量，必须为 2 的幂次
 * @return size_t  映射到 [0, capacity-1] 范围内的桶索引
 *
 * @note   取指纹前 8 字节作为 splitmix64 的输入，再与 capacity-1 做位与运算。
 *         要求 capacity 为 2 的幂，以保证均匀分布。
 */
static inline size_t fp_hash(const uint8_t md5[FP_SIZE], size_t capacity) {
    uint64_t x;
    memcpy(&x, md5, sizeof(x));
    return (size_t)(splitmix64(x) & (capacity - 1));
}

/**
 * @brief  计算不小于 n 的最小 2 的幂次
 * @param  n  size_t  目标下限值，取值范围: >= 0
 * @return size_t  不小于 n 的最小 2 的幂次（如 n=5 返回 8，n=16 返回 16）
 *
 * @note   当 n 本身为 0 时返回 1。本函数用于哈希表初始容量和扩容时的对齐计算。
 */
static size_t next_pow2(size_t n) {
    size_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

/**
 * @brief  创建 ReferenceMap 实例
 * @param  expected_count  size_t  预估元素数量，取值范围: > 0
 * @return ReferenceMap*  成功返回指向新分配映射表的指针；内存不足时返回 NULL
 *
 * @note   实际分配容量为 next_pow2(expected_count * 2)，且最小为 16。
 *         内部使用 calloc 分配 meta 和 entries 数组，初始状态均为空。
 */
ReferenceMap* ref_map_create(size_t expected_count) {
    ReferenceMap *map = malloc(sizeof(ReferenceMap));
    if (!map) return NULL;

    size_t cap = next_pow2(expected_count * 2);
    if (cap < 16) cap = 16;

    map->meta = calloc(cap, sizeof(uint8_t));
    map->entries = calloc(cap, sizeof(ReferenceEntry));
    if (!map->meta || !map->entries) {
        free(map->meta);
        free(map->entries);
        free(map);
        return NULL;
    }
    map->capacity = cap;
    map->count = 0;
    return map;
}

/**
 * @brief  销毁 ReferenceMap 实例并释放所有内部内存
 * @param  map  ReferenceMap*  要销毁的映射表指针，允许传入 NULL（空操作）
 * @return void
 */
void ref_map_destroy(ReferenceMap *map) {
    if (!map) return;
    free(map->meta);
    free(map->entries);
    free(map);
}

/**
 * @brief  向映射表中插入或更新一条指纹记录
 * @param  map     ReferenceMap*           目标映射表指针，不能为空
 * @param  fp      const uint8_t[FP_SIZE]  16 字节文件指纹，不能为空
 * @param  mtime   time_t                  文件最后修改时间，取值范围: 有效 Unix 时间戳
 * @param  d_type  uint8_t                 文件类型（DT_REG/DT_DIR/DT_LNK 等），取值范围: linux/dirent.h 中定义的 d_type 常量
 * @return void
 *
 * @note   若指纹已存在，则覆盖更新其 mtime 和 d_type。
 *         当负载因子超过 0.75（count*2 >= capacity*3）时自动扩容至 2 倍容量，
 *         并重新哈希所有已有条目。
 */
void ref_map_insert(ReferenceMap *map, const uint8_t fp[FP_SIZE], time_t mtime, uint8_t d_type) {
    /* 是否需要扩容 */
    if (map->count * 2 >= map->capacity * 3) {
        size_t old_cap = map->capacity;
        uint8_t *old_meta = map->meta;
        ReferenceEntry *old_entries = map->entries;

        size_t new_cap = old_cap << 1;
        map->meta = calloc(new_cap, sizeof(uint8_t));
        map->entries = calloc(new_cap, sizeof(ReferenceEntry));
        map->capacity = new_cap;
        map->count = 0;

        for (size_t i = 0; i < old_cap; i++) {
            if (old_meta[i] == 1) {
                ref_map_insert(map, old_entries[i].fingerprint,
                               old_entries[i].mtime, old_entries[i].d_type);
            }
        }
        free(old_meta);
        free(old_entries);
    }

    size_t idx = fp_hash(fp, map->capacity);
    for (size_t i = 0; i < map->capacity; i++) {
        size_t pos = (idx + i) & (map->capacity - 1);
        uint8_t m = map->meta[pos];

        if (m == 0) {
            /* 空槽，插入 */
            memcpy(map->entries[pos].fingerprint, fp, FP_SIZE);
            map->entries[pos].mtime = mtime;
            map->entries[pos].d_type = d_type;
            map->meta[pos] = 1;
            map->count++;
            return;
        }
        if (m == 1 && memcmp(map->entries[pos].fingerprint, fp, FP_SIZE) == 0) {
            /* 已存在，覆盖更新（mtime/d_type 可能变化） */
            map->entries[pos].mtime = mtime;
            map->entries[pos].d_type = d_type;
            return;
        }
    }
}

/**
 * @brief  在映射表中查找指定指纹对应的记录
 * @param  map  const ReferenceMap*     目标映射表指针，不能为空
 * @param  fp   const uint8_t[FP_SIZE]  要查找的 16 字节指纹，不能为空
 * @return const ReferenceEntry*  找到时返回指向对应条目的只读指针；未找到时返回 NULL
 *
 * @note   返回的指针指向映射表内部存储，调用方不应修改或释放该指针。
 *         若后续执行了 ref_map_insert 导致扩容，该指针将失效。
 */
const ReferenceEntry* ref_map_lookup(const ReferenceMap *map, const uint8_t fp[FP_SIZE]) {
    size_t idx = fp_hash(fp, map->capacity);
    for (size_t i = 0; i < map->capacity; i++) {
        size_t pos = (idx + i) & (map->capacity - 1);
        uint8_t m = map->meta[pos];
        if (m == 0) return NULL;
        if (m == 1 && memcmp(map->entries[pos].fingerprint, fp, FP_SIZE) == 0)
            return &map->entries[pos];
    }
    return NULL;
}

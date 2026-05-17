/**
 * @file fingerprint_set.c
 * @brief xxHash3 128-bit 分片开放寻址哈希集合实现
 *
 * 采用 64 分片（shard）+ 每分片独立开放寻址（线性探测）的结构，
 * 每个分片拥有独立的 pthread_mutex_t，将全局锁竞争分散到 64 把细粒度锁上，
 * 支持高并发场景下去重与存在性判断（visited_set / reference_set）。
 *
 * 指纹计算基于 xxHash3 128-bit，输入为 path + dev + ino 的拼接数据。
 */
#include "fingerprint_set.h"
#define XXH_STATIC_LINKING_ONLY
#include "xxhash.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ================================================================
 * xxHash3 指纹计算
 * ================================================================ */

/**
 * @brief  计算文件/目录的 128-bit 指纹
 * @param  path  const char*  文件绝对路径，允许为 NULL（仅对 dev+ino 做哈希）
 * @param  dev   uint64_t     文件所在设备号，取值范围: 任意 64-bit 无符号整数
 * @param  ino   uint64_t     文件的 inode 号，取值范围: 任意 64-bit 无符号整数
 * @param  out   uint8_t[FP_SIZE]  输出缓冲区，长度为 16 字节，用于存放计算后的指纹
 * @return void
 *
 * @note   使用 xxHash3 128-bit 算法，依次 update path、dev、ino，
 *         最终输出规范化的 16 字节摘要。该指纹用于全局去重和半增量索引。
 */
void fp_compute(const char *path, uint64_t dev, uint64_t ino, uint8_t out[FP_SIZE]) {
    XXH3_state_t state;
    XXH128_canonical_t canonical;
    XXH3_128bits_reset(&state);
    if (path) XXH3_128bits_update(&state, path, strlen(path));
    XXH3_128bits_update(&state, &dev, sizeof(dev));
    XXH3_128bits_update(&state, &ino, sizeof(ino));
    XXH128_canonicalFromHash(&canonical, XXH3_128bits_digest(&state));
    memcpy(out, canonical.digest, FP_SIZE);
}

/* ================================================================
 * FingerprintSet 实现 — 分片开放寻址法 + 线性探测
 * ================================================================ */

/**
 * @brief  splitmix64 伪随机数生成/哈希混淆函数
 * @param  x  uint64_t  输入种子值，取值范围: 任意 64-bit 无符号整数
 * @return uint64_t  经过多轮位运算混淆后的输出值
 */
static inline uint64_t splitmix64(uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x  = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x  = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

/**
 * @brief  计算指纹在单个分片内的哈希桶索引
 * @param  md5       const uint8_t[FP_SIZE]  16 字节指纹数据
 * @param  capacity  size_t                   分片当前容量，必须为 2 的幂次
 * @return size_t  映射到 [0, capacity-1] 范围内的桶索引
 */
static inline size_t fp_hash(const uint8_t md5[FP_SIZE], size_t capacity) {
    uint64_t x;
    memcpy(&x, md5, sizeof(x));
    return (size_t)(splitmix64(x) & (capacity - 1));
}

/**
 * @brief  根据指纹值计算所属的分片索引
 * @param  md5  const uint8_t[FP_SIZE]  16 字节指纹数据
 * @return size_t  分片索引，取值范围: [0, FP_SHARD_COUNT-1]（即 [0, 63]）
 *
 * @note   取指纹前 8 字节的高 6 位（bit 58-63）作为分片索引，
 *         使指纹天然均匀散布到 64 个分片上，无需额外哈希。
 */
static inline size_t fp_shard_index(const uint8_t md5[FP_SIZE]) {
    uint64_t x;
    memcpy(&x, md5, sizeof(x));
    return (size_t)(x >> 58) & (FP_SHARD_COUNT - 1);
}

/**
 * @brief  计算不小于 n 的最小 2 的幂次
 * @param  n  size_t  目标下限值，取值范围: >= 0
 * @return size_t  不小于 n 的最小 2 的幂次
 */
static size_t next_pow2(size_t n) {
    size_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

/**
 * @brief  向指定分片内部插入指纹（无锁，调用方必须已持有该分片的 mutex）
 * @param  shard       FingerprintShard*  目标分片指针，不能为空
 * @param  md5         const uint8_t[FP_SIZE]  要插入的 16 字节指纹
 * @param  out_exists  bool*  输出参数，返回 true 表示指纹已存在；false 表示新插入
 * @return bool  操作是否成功。正常情况下始终返回 true；理论上不应返回 false。
 *
 * @note   当分片负载因子超过 0.75 时自动扩容至 2 倍。
 *         使用线性探测解决冲突；遇到 tombstone（删除标记）时会复用该槽位。
 */
static bool fp_shard_insert_internal(FingerprintShard *shard, const uint8_t md5[FP_SIZE], bool *out_exists) {
    /* v15.1.3: capacity sanity check */
    if (shard->capacity == 0 || shard->capacity > (1ULL << 30)) {
        log_fatal("[FPSet] shard capacity corrupted: %zu (valid range: [16, 1<<30])", shard->capacity);
        return false;
    }
    if ((shard->capacity & (shard->capacity - 1)) != 0) {
        log_fatal("[FPSet] shard capacity not power of 2: %zu", shard->capacity);
        return false;
    }

    if ((shard->count + shard->tombstones) * 2 >= shard->capacity * 3) {
        /* 扩容到 2 倍 */
        size_t old_cap = shard->capacity;
        uint8_t *old_meta = shard->meta;
        Fingerprint *old_table = shard->table;

        size_t new_cap = old_cap << 1;
        if (new_cap > (1ULL << 30)) {
            log_fatal("[FPSet] shard capacity overflow during resize: %zu -> %zu", old_cap, new_cap);
            return false;
        }
        shard->meta = calloc(new_cap, sizeof(uint8_t));
        shard->table = calloc(new_cap, sizeof(Fingerprint));
        if (!shard->meta || !shard->table) {
            log_fatal("[FPSet] shard resize allocation failed: new_cap=%zu", new_cap);
            free(shard->meta);
            free(shard->table);
            shard->meta = old_meta;
            shard->table = old_table;
            return false;
        }
        shard->capacity = new_cap;
        shard->count = 0;
        shard->tombstones = 0;

        for (size_t i = 0; i < old_cap; i++) {
            if (old_meta[i] == 1) {
                bool ignored;
                fp_shard_insert_internal(shard, old_table[i].md5, &ignored);
            }
        }
        free(old_meta);
        free(old_table);
    }

    size_t idx = fp_hash(md5, shard->capacity);
    size_t first_tomb = (size_t)-1;
    const size_t PROBE_LIMIT = 1000000; /* v15.1.3: hard limit to prevent infinite loop */

    for (size_t i = 0; i < shard->capacity; i++) {
        if (i >= PROBE_LIMIT) {
            log_fatal("[FPSet] open-addressing probe limit exceeded: capacity=%zu, count=%zu, tombstones=%zu",
                      shard->capacity, shard->count, shard->tombstones);
            return false;
        }
        size_t pos = (idx + i) & (shard->capacity - 1);
        uint8_t m = shard->meta[pos];

        if (m == 0) {
            if (first_tomb != (size_t)-1) pos = first_tomb;
            memcpy(shard->table[pos].md5, md5, FP_SIZE);
            shard->meta[pos] = 1;
            shard->count++;
            *out_exists = false;
            return true;
        }
        if (m == 2 && first_tomb == (size_t)-1) {
            first_tomb = pos;
        }
        if (m == 1 && memcmp(shard->table[pos].md5, md5, FP_SIZE) == 0) {
            *out_exists = true;
            return true;
        }
    }
    /* v15.1.3: should never reach here under normal conditions */
    log_fatal("[FPSet] open-addressing loop exhausted capacity=%zu (count=%zu, tombstones=%zu). Table full or corrupted.",
              shard->capacity, shard->count, shard->tombstones);
    return false; /* 不应该到达这里 */
}

/**
 * @brief  创建 FingerprintSet 实例
 * @param  expected_count  size_t  预估全局元素总数量，取值范围: > 0
 * @return FingerprintSet*  成功返回指向新分配集合的指针；内存不足时返回 NULL
 *
 * @note   总容量按 expected_count * 2 均摊到 64 个分片，每分片独立分配。
 *         每个分片的最小容量为 16。若某分片分配失败，则回滚并释放已分配的分片资源。
 */
FingerprintSet* fp_set_create(size_t expected_count) {
    FingerprintSet *set = malloc(sizeof(FingerprintSet));
    if (!set) return NULL;

    size_t per_shard = next_pow2((expected_count * 2 + FP_SHARD_COUNT - 1) / FP_SHARD_COUNT);
    if (per_shard < 16) per_shard = 16;

    for (int s = 0; s < FP_SHARD_COUNT; s++) {
        FingerprintShard *shard = &set->shards[s];
        shard->meta = calloc(per_shard, sizeof(uint8_t));
        shard->table = calloc(per_shard, sizeof(Fingerprint));
        if (!shard->meta || !shard->table) {
            for (int j = 0; j <= s; j++) {
                free(set->shards[j].meta);
                free(set->shards[j].table);
                if (j < s) pthread_mutex_destroy(&set->shards[j].mutex);
            }
            free(set);
            return NULL;
        }
        shard->capacity = per_shard;
        shard->count = 0;
        shard->tombstones = 0;
        pthread_mutex_init(&shard->mutex, NULL);
    }
    return set;
}

/**
 * @brief  销毁 FingerprintSet 实例并释放所有内部内存
 * @param  set  FingerprintSet*  要销毁的集合指针，允许传入 NULL（空操作）
 * @return void
 */
void fp_set_destroy(FingerprintSet *set) {
    if (!set) return;
    for (int s = 0; s < FP_SHARD_COUNT; s++) {
        FingerprintShard *shard = &set->shards[s];
        free(shard->meta);
        free(shard->table);
        pthread_mutex_destroy(&shard->mutex);
    }
    free(set);
}

/**
 * @brief  向集合中插入一个指纹
 * @param  set  FingerprintSet*        目标集合指针，不能为空
 * @param  md5  const uint8_t[FP_SIZE]  要插入的 16 字节指纹
 * @return bool  返回 true 表示该指纹已存在于集合中；false 表示新插入成功
 *
 * @note   操作过程自动定位到对应分片并加锁，线程安全。
 *         分片内可能触发自动扩容，但扩容期间仍持有该分片锁。
 */
bool fp_set_insert(FingerprintSet *set, const uint8_t md5[FP_SIZE]) {
    size_t si = fp_shard_index(md5);
    FingerprintShard *shard = &set->shards[si];
    pthread_mutex_lock(&shard->mutex);
    bool exists = false;
    fp_shard_insert_internal(shard, md5, &exists);
    pthread_mutex_unlock(&shard->mutex);
    return exists;
}

/**
 * @brief  判断集合中是否包含指定指纹
 * @param  set  const FingerprintSet*  目标集合指针，不能为空
 * @param  md5  const uint8_t[FP_SIZE]  要查询的 16 字节指纹
 * @return bool  返回 true 表示指纹存在于集合中；false 表示不存在
 *
 * @note   操作过程自动定位到对应分片并加锁，线程安全。
 *         仅做只读查询，不会修改集合状态，也不会触发扩容。
 */
bool fp_set_contains(const FingerprintSet *set, const uint8_t md5[FP_SIZE]) {
    size_t si = fp_shard_index(md5);
    const FingerprintShard *shard = &set->shards[si];
    pthread_mutex_lock((pthread_mutex_t *)&shard->mutex);
    size_t idx = fp_hash(md5, shard->capacity);
    bool found = false;
    for (size_t i = 0; i < shard->capacity; i++) {
        size_t pos = (idx + i) & (shard->capacity - 1);
        uint8_t m = shard->meta[pos];
        if (m == 0) break;
        if (m == 1 && memcmp(shard->table[pos].md5, md5, FP_SIZE) == 0) {
            found = true;
            break;
        }
    }
    pthread_mutex_unlock((pthread_mutex_t *)&shard->mutex);
    return found;
}

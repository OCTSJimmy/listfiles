#include "fingerprint_set.h"
#define XXH_STATIC_LINKING_ONLY
#include "xxhash.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ================================================================
 * xxHash3 指纹计算
 * ================================================================ */

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

static inline uint64_t splitmix64(uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x  = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x  = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

static inline size_t fp_hash(const uint8_t md5[FP_SIZE], size_t capacity) {
    uint64_t x;
    memcpy(&x, md5, sizeof(x));
    return (size_t)(splitmix64(x) & (capacity - 1));
}

static inline size_t fp_shard_index(const uint8_t md5[FP_SIZE]) {
    uint64_t x;
    memcpy(&x, md5, sizeof(x));
    return (size_t)(x >> 58) & (FP_SHARD_COUNT - 1);
}

static size_t next_pow2(size_t n) {
    size_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

static bool fp_shard_insert_internal(FingerprintShard *shard, const uint8_t md5[FP_SIZE], bool *out_exists) {
    if ((shard->count + shard->tombstones) * 2 >= shard->capacity * 3) {
        /* 扩容到 2 倍 */
        size_t old_cap = shard->capacity;
        uint8_t *old_meta = shard->meta;
        Fingerprint *old_table = shard->table;

        size_t new_cap = old_cap << 1;
        shard->meta = calloc(new_cap, sizeof(uint8_t));
        shard->table = calloc(new_cap, sizeof(Fingerprint));
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

    for (size_t i = 0; i < shard->capacity; i++) {
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
    return false; /* 不应该到达这里 */
}

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

bool fp_set_insert(FingerprintSet *set, const uint8_t md5[FP_SIZE]) {
    size_t si = fp_shard_index(md5);
    FingerprintShard *shard = &set->shards[si];
    pthread_mutex_lock(&shard->mutex);
    bool exists = false;
    fp_shard_insert_internal(shard, md5, &exists);
    pthread_mutex_unlock(&shard->mutex);
    return exists;
}

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

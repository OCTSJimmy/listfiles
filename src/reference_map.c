#include "reference_map.h"
#include <stdlib.h>
#include <string.h>

/* 与 fingerprint_set.c 使用完全相同的哈希函数 */
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

static size_t next_pow2(size_t n) {
    size_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

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

void ref_map_destroy(ReferenceMap *map) {
    if (!map) return;
    free(map->meta);
    free(map->entries);
    free(map);
}

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

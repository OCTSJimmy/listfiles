#ifndef FINGERPRINT_SET_H
#define FINGERPRINT_SET_H

#include <stdint.h>
#include <stdbool.h>

#define FP_SIZE 16

typedef struct {
    uint8_t md5[FP_SIZE];
} Fingerprint;

typedef struct {
    uint8_t *meta;        /* 0=empty, 1=occupied, 2=tombstone */
    Fingerprint *table;
    size_t capacity;
    size_t count;
    size_t tombstones;
} FingerprintSet;

FingerprintSet* fp_set_create(size_t expected_count);
void fp_set_destroy(FingerprintSet *set);

/* 返回 true 表示已存在，false 表示新插入 */
bool fp_set_insert(FingerprintSet *set, const uint8_t md5[FP_SIZE]);
bool fp_set_contains(const FingerprintSet *set, const uint8_t md5[FP_SIZE]);

/* 计算指纹: MD5(path + dev + ino) */
void fp_compute(const char *path, uint64_t dev, uint64_t ino, uint8_t out[FP_SIZE]);

#endif

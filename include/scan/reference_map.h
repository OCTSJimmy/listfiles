#ifndef REFERENCE_MAP_H
#define REFERENCE_MAP_H

#include <stdint.h>
#include <time.h>
#include <sys/types.h>
#include "fingerprint_set.h"

/* v15.6.0（P0-011 / §0.2/§0.3）：盲信基准条目保存完整历史 stat
 * （mtime/mtime_nsec/size/uid/gid/mode/d_type 全部来自基准 pbin schema 2 记录），
 * 命中后输出不再退化零字段。key 为纯路径哈希 xxHash3-128(path)（fp_compute(path,0,0)，
 * 不带 dev/ino——盲信不 lstat 拿不到）。 */
typedef struct {
    uint8_t fingerprint[FP_SIZE];
    time_t  mtime;        /* 基准记录的 mtime（秒） */
    long    mtime_nsec;   /* 基准记录的 mtime 纳秒部分 */
    off_t   size;         /* 基准记录的文件大小 */
    uint32_t uid;
    uint32_t gid;
    uint32_t mode;        /* 完整 st_mode（类型位 + 权限位） */
    uint8_t d_type;
    uint8_t _pad[7];
} ReferenceEntry;

typedef struct {
    uint8_t *meta;        /* 0=empty, 1=occupied */
    ReferenceEntry *entries;
    size_t capacity;
    size_t count;
} ReferenceMap;

ReferenceMap* ref_map_create(size_t expected_count);
void ref_map_destroy(ReferenceMap *map);

void ref_map_insert(ReferenceMap *map, const uint8_t fp[FP_SIZE],
                    time_t mtime, long mtime_nsec, off_t size,
                    uint32_t uid, uint32_t gid, uint32_t mode, uint8_t d_type);
const ReferenceEntry* ref_map_lookup(const ReferenceMap *map, const uint8_t fp[FP_SIZE]);

#endif

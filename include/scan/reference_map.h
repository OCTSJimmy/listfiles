#ifndef REFERENCE_MAP_H
#define REFERENCE_MAP_H

#include <stdint.h>
#include <time.h>
#include "fingerprint_set.h"

typedef struct {
    uint8_t fingerprint[FP_SIZE];
    time_t  mtime;
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

void ref_map_insert(ReferenceMap *map, const uint8_t fp[FP_SIZE], time_t mtime, uint8_t d_type);
const ReferenceEntry* ref_map_lookup(const ReferenceMap *map, const uint8_t fp[FP_SIZE]);

#endif

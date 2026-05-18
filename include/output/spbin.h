#ifndef SPBIN_H
#define SPBIN_H

#include <stdint.h>
#include <time.h>

#define SP_STATUS_PROBING   0
#define SP_STATUS_CONDEMNED 1

/* Disk record header; path bytes follow immediately */
typedef struct __attribute__((packed)) {
    uint32_t path_len;
    uint64_t dev;
    time_t   blacklist_time;
    uint32_t retry_count;
    uint32_t probe_interval;
    uint8_t  d_type;
    uint8_t  s_status;
} SpbinRecordHeader;

/* In-memory representation */
typedef struct {
    char    *path;
    uint64_t dev;
    time_t   blacklist_time;
    uint32_t retry_count;
    uint32_t probe_interval;
    uint8_t  d_type;
    uint8_t  s_status;
} SpbinEntry;

#endif

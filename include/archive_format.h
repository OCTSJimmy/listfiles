#ifndef ARCHIVE_FORMAT_H
#define ARCHIVE_FORMAT_H

#include <stdint.h>

#define ARCHIVE_BLOCK_NORMAL 0
#define ARCHIVE_BLOCK_SPBIN  1

/* Pbin / fpbin 通用页脚（自描述） */
typedef struct __attribute__((packed)) {
    uint64_t magic;        /* 0xDEADBEEF66AAC0FF */
    uint64_t row_count;    /* 该分片实际总行数 */
    uint32_t data_crc32;   /* 预留：覆盖数据区的 CRC（当前填 0） */
    uint32_t footer_crc32; /* 覆盖 Footer 前 16 字节（magic + row_count）的 CRC32 */
} PbinFooter;

typedef struct __attribute__((packed)) {
    uint32_t uncompressed_size;
    uint32_t compressed_size;
    uint8_t  block_type;       /* 0=normal pbin, 1=spbin */
    uint64_t row_count;        /* 对应分片的数据行数 */
} ArchiveBlockHeader;

#endif

#ifndef ARCHIVE_FORMAT_H
#define ARCHIVE_FORMAT_H

#include <stdint.h>

#define ARCHIVE_BLOCK_NORMAL 0
#define ARCHIVE_BLOCK_SPBIN  1

typedef struct __attribute__((packed)) {
    uint32_t uncompressed_size;
    uint32_t compressed_size;
    uint8_t  block_type;       /* 0=normal pbin, 1=spbin */
} ArchiveBlockHeader;

#endif

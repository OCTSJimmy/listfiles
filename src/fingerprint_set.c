#include "fingerprint_set.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ================================================================
 * 内嵌公共域 MD5 实现 (RFC 1321)
 * ================================================================ */
typedef struct {
    uint32_t state[4];
    uint32_t count[2];
    uint8_t  buffer[64];
} MD5_CTX;

static void md5_encode(uint8_t *output, const uint32_t *input, size_t len) {
    for (size_t i = 0, j = 0; j < len; i++, j += 4) {
        output[j]   = (uint8_t)(input[i] & 0xff);
        output[j+1] = (uint8_t)((input[i] >> 8) & 0xff);
        output[j+2] = (uint8_t)((input[i] >> 16) & 0xff);
        output[j+3] = (uint8_t)((input[i] >> 24) & 0xff);
    }
}

static void md5_decode(uint32_t *output, const uint8_t *input, size_t len) {
    for (size_t i = 0, j = 0; j < len; i++, j += 4) {
        output[i] = ((uint32_t)input[j])
                  | (((uint32_t)input[j+1]) << 8)
                  | (((uint32_t)input[j+2]) << 16)
                  | (((uint32_t)input[j+3]) << 24);
    }
}

static void md5_transform(uint32_t state[4], const uint8_t block[64]) {
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t x[16];
    md5_decode(x, block, 64);

    #define F(x,y,z) (((x) & (y)) | ((~x) & (z)))
    #define G(x,y,z) (((x) & (z)) | ((y) & (~z)))
    #define H(x,y,z) ((x) ^ (y) ^ (z))
    #define I(x,y,z) ((y) ^ ((x) | (~z)))
    #define ROTATE_LEFT(x,n) (((x) << (n)) | ((x) >> (32-(n))))
    #define FF(a,b,c,d,x,s,ac) { (a) += F((b),(c),(d)) + (x) + (uint32_t)(ac); (a) = ROTATE_LEFT((a),(s)); (a) += (b); }
    #define GG(a,b,c,d,x,s,ac) { (a) += G((b),(c),(d)) + (x) + (uint32_t)(ac); (a) = ROTATE_LEFT((a),(s)); (a) += (b); }
    #define HH(a,b,c,d,x,s,ac) { (a) += H((b),(c),(d)) + (x) + (uint32_t)(ac); (a) = ROTATE_LEFT((a),(s)); (a) += (b); }
    #define II(a,b,c,d,x,s,ac) { (a) += I((b),(c),(d)) + (x) + (uint32_t)(ac); (a) = ROTATE_LEFT((a),(s)); (a) += (b); }

    FF(a,b,c,d,x[ 0], 7,0xd76aa478); FF(d,a,b,c,x[ 1],12,0xe8c7b756);
    FF(c,d,a,b,x[ 2],17,0x242070db); FF(b,c,d,a,x[ 3],22,0xc1bdceee);
    FF(a,b,c,d,x[ 4], 7,0xf57c0faf); FF(d,a,b,c,x[ 5],12,0x4787c62a);
    FF(c,d,a,b,x[ 6],17,0xa8304613); FF(b,c,d,a,x[ 7],22,0xfd469501);
    FF(a,b,c,d,x[ 8], 7,0x698098d8); FF(d,a,b,c,x[ 9],12,0x8b44f7af);
    FF(c,d,a,b,x[10],17,0xffff5bb1); FF(b,c,d,a,x[11],22,0x895cd7be);
    FF(a,b,c,d,x[12], 7,0x6b901122); FF(d,a,b,c,x[13],12,0xfd987193);
    FF(c,d,a,b,x[14],17,0xa679438e); FF(b,c,d,a,x[15],22,0x49b40821);

    GG(a,b,c,d,x[ 1], 5,0xf61e2562); GG(d,a,b,c,x[ 6], 9,0xc040b340);
    GG(c,d,a,b,x[11],14,0x265e5a51); GG(b,c,d,a,x[ 0],20,0xe9b6c7aa);
    GG(a,b,c,d,x[ 5], 5,0xd62f105d); GG(d,a,b,c,x[10], 9,0x02441453);
    GG(c,d,a,b,x[15],14,0xd8a1e681); GG(b,c,d,a,x[ 4],20,0xe7d3fbc8);
    GG(a,b,c,d,x[ 9], 5,0x21e1cde6); GG(d,a,b,c,x[14], 9,0xc33707d6);
    GG(c,d,a,b,x[ 3],14,0xf4d50d87); GG(b,c,d,a,x[ 8],20,0x455a14ed);
    GG(a,b,c,d,x[13], 5,0xa9e3e905); GG(d,a,b,c,x[ 2], 9,0xfcefa3f8);
    GG(c,d,a,b,x[ 7],14,0x676f02d9); GG(b,c,d,a,x[12],20,0x8d2a4c8a);

    HH(a,b,c,d,x[ 5], 4,0xfffa3942); HH(d,a,b,c,x[ 8],11,0x8771f681);
    HH(c,d,a,b,x[11],16,0x6d9d6122); HH(b,c,d,a,x[14],23,0xfde5380c);
    HH(a,b,c,d,x[ 1], 4,0xa4beea44); HH(d,a,b,c,x[ 4],11,0x4bdecfa9);
    HH(c,d,a,b,x[ 7],16,0xf6bb4b60); HH(b,c,d,a,x[10],23,0xbebfbc70);
    HH(a,b,c,d,x[13], 4,0x289b7ec6); HH(d,a,b,c,x[ 0],11,0xeaa127fa);
    HH(c,d,a,b,x[ 3],16,0xd4ef3085); HH(b,c,d,a,x[ 6],23,0x04881d05);
    HH(a,b,c,d,x[ 9], 4,0xd9d4d039); HH(d,a,b,c,x[12],11,0xe6db99e5);
    HH(c,d,a,b,x[15],16,0x1fa27cf8); HH(b,c,d,a,x[ 2],23,0xc4ac5665);

    II(a,b,c,d,x[ 0], 6,0xf4292244); II(d,a,b,c,x[ 7],10,0x432aff97);
    II(c,d,a,b,x[14],15,0xab9423a7); II(b,c,d,a,x[ 5],21,0xfc93a039);
    II(a,b,c,d,x[12], 6,0x655b59c3); II(d,a,b,c,x[ 3],10,0x8f0ccc92);
    II(c,d,a,b,x[10],15,0xffeff47d); II(b,c,d,a,x[ 1],21,0x85845dd1);
    II(a,b,c,d,x[ 8], 6,0x6fa87e4f); II(d,a,b,c,x[15],10,0xfe2ce6e0);
    II(c,d,a,b,x[ 6],15,0xa3014314); II(b,c,d,a,x[13],21,0x4e0811a1);
    II(a,b,c,d,x[ 4], 6,0xf7537e82); II(d,a,b,c,x[11],10,0xbd3af235);
    II(c,d,a,b,x[ 2],15,0x2ad7d2bb); II(b,c,d,a,x[ 9],21,0xeb86d391);

    #undef F
    #undef G
    #undef H
    #undef I
    #undef ROTATE_LEFT
    #undef FF
    #undef GG
    #undef HH
    #undef II

    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    memset(x, 0, sizeof(x));
}

static void MD5_Init(MD5_CTX *ctx) {
    ctx->count[0] = ctx->count[1] = 0;
    ctx->state[0] = 0x67452301;
    ctx->state[1] = 0xEFCDAB89;
    ctx->state[2] = 0x98BADCFE;
    ctx->state[3] = 0x10325476;
}

static void MD5_Update(MD5_CTX *ctx, const uint8_t *input, size_t inputLen) {
    uint32_t i, index, partLen;
    index = (uint32_t)((ctx->count[0] >> 3) & 0x3F);
    if ((ctx->count[0] += ((uint32_t)inputLen << 3)) < ((uint32_t)inputLen << 3))
        ctx->count[1]++;
    ctx->count[1] += ((uint32_t)inputLen >> 29);
    partLen = 64 - index;

    if (inputLen >= partLen) {
        memcpy(&ctx->buffer[index], input, partLen);
        md5_transform(ctx->state, ctx->buffer);
        for (i = partLen; i + 63 < inputLen; i += 64)
            md5_transform(ctx->state, &input[i]);
        index = 0;
    } else {
        i = 0;
    }
    memcpy(&ctx->buffer[index], &input[i], inputLen - i);
}

static void MD5_Final(uint8_t digest[16], MD5_CTX *ctx) {
    uint8_t bits[8];
    uint32_t index, padLen;
    static const uint8_t PADDING[64] = {
        0x80,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
    };
    md5_encode(bits, ctx->count, 8);
    index = (uint32_t)((ctx->count[0] >> 3) & 0x3f);
    padLen = (index < 56) ? (56 - index) : (120 - index);
    MD5_Update(ctx, PADDING, padLen);
    MD5_Update(ctx, bits, 8);
    md5_encode(digest, ctx->state, 16);
    memset(ctx, 0, sizeof(*ctx));
}

/* ================================================================
 * FingerprintSet 实现 — 开放寻址法 + 线性探测
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

static size_t next_pow2(size_t n) {
    size_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

FingerprintSet* fp_set_create(size_t expected_count) {
    FingerprintSet *set = malloc(sizeof(FingerprintSet));
    if (!set) return NULL;

    size_t cap = next_pow2(expected_count * 2);
    if (cap < 16) cap = 16;

    set->meta = calloc(cap, sizeof(uint8_t));
    set->table = calloc(cap, sizeof(Fingerprint));
    if (!set->meta || !set->table) {
        free(set->meta);
        free(set->table);
        free(set);
        return NULL;
    }
    set->capacity = cap;
    set->count = 0;
    set->tombstones = 0;
    return set;
}

void fp_set_destroy(FingerprintSet *set) {
    if (!set) return;
    free(set->meta);
    free(set->table);
    free(set);
}

static bool fp_set_insert_internal(FingerprintSet *set, const uint8_t md5[FP_SIZE], bool *out_exists) {
    if ((set->count + set->tombstones) * 2 >= set->capacity * 3) {
        /* 扩容到 2 倍 */
        size_t old_cap = set->capacity;
        uint8_t *old_meta = set->meta;
        Fingerprint *old_table = set->table;

        size_t new_cap = old_cap << 1;
        set->meta = calloc(new_cap, sizeof(uint8_t));
        set->table = calloc(new_cap, sizeof(Fingerprint));
        set->capacity = new_cap;
        set->count = 0;
        set->tombstones = 0;

        for (size_t i = 0; i < old_cap; i++) {
            if (old_meta[i] == 1) {
                bool ignored;
                fp_set_insert_internal(set, old_table[i].md5, &ignored);
            }
        }
        free(old_meta);
        free(old_table);
    }

    size_t idx = fp_hash(md5, set->capacity);
    size_t first_tomb = (size_t)-1;

    for (size_t i = 0; i < set->capacity; i++) {
        size_t pos = (idx + i) & (set->capacity - 1);
        uint8_t m = set->meta[pos];

        if (m == 0) {
            /* 空槽 */
            if (first_tomb != (size_t)-1) pos = first_tomb;
            memcpy(set->table[pos].md5, md5, FP_SIZE);
            set->meta[pos] = 1;
            set->count++;
            *out_exists = false;
            return true;
        }
        if (m == 2 && first_tomb == (size_t)-1) {
            first_tomb = pos;
        }
        if (m == 1 && memcmp(set->table[pos].md5, md5, FP_SIZE) == 0) {
            *out_exists = true;
            return true;
        }
    }
    return false; /* 不应该到达这里 */
}

bool fp_set_insert(FingerprintSet *set, const uint8_t md5[FP_SIZE]) {
    bool exists = false;
    fp_set_insert_internal(set, md5, &exists);
    return exists;
}

bool fp_set_contains(const FingerprintSet *set, const uint8_t md5[FP_SIZE]) {
    size_t idx = fp_hash(md5, set->capacity);
    for (size_t i = 0; i < set->capacity; i++) {
        size_t pos = (idx + i) & (set->capacity - 1);
        uint8_t m = set->meta[pos];
        if (m == 0) return false;
        if (m == 1 && memcmp(set->table[pos].md5, md5, FP_SIZE) == 0) return true;
    }
    return false;
}

void fp_compute(const char *path, uint64_t dev, uint64_t ino, uint8_t out[FP_SIZE]) {
    MD5_CTX ctx;
    MD5_Init(&ctx);
    if (path) MD5_Update(&ctx, (const uint8_t*)path, strlen(path));
    MD5_Update(&ctx, (const uint8_t*)&dev, sizeof(dev));
    MD5_Update(&ctx, (const uint8_t*)&ino, sizeof(ino));
    MD5_Final(out, &ctx);
}

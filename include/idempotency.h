#ifndef IDEMPOTENCY_H
#define IDEMPOTENCY_H

#include <sys/types.h>
#include <sys/stat.h>
#include <stdint.h>
#include <time.h>
#include "config.h"

// [升级] 对象标识符结构体
// 增加了用于半增量校验的关键字段
typedef struct {
    dev_t st_dev;
    ino_t st_ino;
    time_t mtime;       // [新增] 历史修改时间 (用于区间判定)
    uint32_t name_hash; // [新增] 文件名哈希 (CRC32/DJB2, 防 Inode 复用)
    unsigned char d_type; // [新增] 文件类型 (DT_REG/DT_DIR...)
} ObjectIdentifier;

// 哈希集合节点
typedef struct HashSetNode {
    ObjectIdentifier id;
    struct HashSetNode *next;
} HashSetNode;

// 哈希集合主结构
typedef struct {
    HashSetNode **table;
    size_t table_size;
    size_t element_count;
} HashSet;

// [修改] 全局集合：Looper 使用的“已访问”集合 (防环用)
extern HashSet* g_visited_history;

// [新增] 全局集合：Worker 使用的“参考”集合 (半增量用，只读)
extern HashSet* g_reference_history;

// 函数声明
HashSet* hash_set_create(size_t size);
void hash_set_destroy(HashSet *set);
void hash_set_insert(HashSet *set, const ObjectIdentifier *id);
int hash_set_contains(const HashSet *set, const ObjectIdentifier *id);

// [新增] 查找并返回节点 (用于读取历史 mtime/hash)
HashSetNode* hash_set_lookup(const HashSet *set, dev_t dev, ino_t ino);

// [新增] 计算文件名哈希
uint32_t calculate_name_hash(const char *str);

#endif // IDEMPOTENCY_H
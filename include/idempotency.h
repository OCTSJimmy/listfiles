#ifndef IDEMPOTENCY_H
#define IDEMPOTENCY_H

#include <sys/types.h>
#include "config.h"

// 对象标识符结构体
typedef struct {
    dev_t st_dev;
    ino_t st_ino;
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

// 声明一个外部全局变量，用于存储历史对象的标识符集合
// extern 关键字告诉编译器这个变量在其他 .c 文件中定义
extern HashSet* g_history_object_set;

// 函数声明
HashSet* hash_set_create(size_t size);
void hash_set_destroy(HashSet *set);
void hash_set_insert(HashSet *set, const ObjectIdentifier *id);
int hash_set_contains(const HashSet *set, const ObjectIdentifier *id);

#endif // IDEMPOTENCY_H
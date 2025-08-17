#include "idempotency.h"
#include "utils.h" // for safe_malloc
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// 定义在.h文件中声明的全局变量
HashSet* g_history_object_set = NULL;

/**
 * @brief 创建并初始化哈希集合
 */
HashSet* hash_set_create(size_t size) {
    HashSet *set = safe_malloc(sizeof(HashSet));
    set->table_size = size;
    set->element_count = 0;
    set->table = safe_malloc(sizeof(HashSetNode*) * size);
    memset(set->table, 0, sizeof(HashSetNode*) * size);
    return set;
}

/**
 * @brief 哈希函数，将(dev, ino)对映射到哈希表桶索引
 */
static size_t hash_function(const ObjectIdentifier *id, size_t table_size) {
    // 一个简单的组合哈希，将两个64位整数混合
    uint64_t hash = 17;
    hash = hash * 31 + id->st_dev;
    hash = hash * 31 + id->st_ino;
    return hash % table_size;
}


/**
 * @brief 销毁哈希集合，释放所有内存
 */
void hash_set_destroy(HashSet *set) {
    if (!set) return;
    for (size_t i = 0; i < set->table_size; i++) {
        HashSetNode *current = set->table[i];
        while (current) {
            HashSetNode *next = current->next;
            free(current);
            current = next;
        }
    }
    free(set->table);
    free(set);
}

/**
 * @brief 将一个对象标识符插入哈希集合
 */
void hash_set_insert(HashSet *set, const ObjectIdentifier *id) {
    size_t index = hash_function(id, set->table_size);
    HashSetNode *current = set->table[index];
    
    while (current) {
        if (current->id.st_dev == id->st_dev && current->id.st_ino == id->st_ino) {
            return; // 已存在
        }
        current = current->next;
    }

    HashSetNode *newNode = safe_malloc(sizeof(HashSetNode));
    newNode->id = *id;
    newNode->next = set->table[index];
    set->table[index] = newNode;
    set->element_count++;
}

/**
 * @brief 检查一个对象标识符是否存在于哈希集合中
 * @return 1 如果存在, 0 如果不存在
 */
int hash_set_contains(const HashSet *set, const ObjectIdentifier *id) {
    if (!set) return 0;
    size_t index = hash_function(id, set->table_size);
    const HashSetNode *current = set->table[index];
    while (current) {
        if (current->id.st_dev == id->st_dev && current->id.st_ino == id->st_ino) {
            return 1; // 找到匹配
        }
        current = current->next;
    }
    return 0; // 未找到
}
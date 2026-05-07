#include "idempotency.h"
#include "utils.h" // for safe_malloc
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// 定义全局变量
HashSet* g_visited_history = NULL;   // Looper 专用 (防环)
HashSet* g_reference_history = NULL; // Worker 专用 (历史缓存，只读)

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
 * 注意：哈希只依赖 dev 和 ino，不依赖 mtime/hash
 */
static size_t hash_function(const ObjectIdentifier *id, size_t table_size) {
    uint64_t hash = 17;
    hash = hash * 31 + id->st_dev;
    hash = hash * 31 + id->st_ino;
    return hash % table_size;
}

/**
 * @brief 销毁哈希集合
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
 * @brief 插入 (如果存在则更新元数据)
 */
void hash_set_insert(HashSet *set, const ObjectIdentifier *id) {
    size_t index = hash_function(id, set->table_size);
    HashSetNode *current = set->table[index];
    
    while (current) {
        if (current->id.st_dev == id->st_dev && current->id.st_ino == id->st_ino) {
            // [修改] 如果节点已存在，更新元数据 (mtime, hash, type)
            // 这对于 Resume 模式很重要，确保内存中是最新的
            current->id.mtime = id->mtime;
            current->id.name_hash = id->name_hash;
            current->id.d_type = id->d_type;
            return; 
        }
        current = current->next;
    }

    HashSetNode *newNode = safe_malloc(sizeof(HashSetNode));
    newNode->id = *id; // 结构体拷贝
    newNode->next = set->table[index];
    set->table[index] = newNode;
    set->element_count++;
}

/**
 * @brief 检查存在性
 */
int hash_set_contains(const HashSet *set, const ObjectIdentifier *id) {
    if (!set) return 0;
    size_t index = hash_function(id, set->table_size);
    const HashSetNode *current = set->table[index];
    while (current) {
        if (current->id.st_dev == id->st_dev && current->id.st_ino == id->st_ino) {
            return 1; 
        }
        current = current->next;
    }
    return 0;
}

/**
 * @brief [新增] 查找并返回节点 (用于获取历史元数据)
 */
HashSetNode* hash_set_lookup(const HashSet *set, dev_t dev, ino_t ino) {
    if (!set) return NULL;
    ObjectIdentifier temp_id = { .st_dev = dev, .st_ino = ino };
    size_t index = hash_function(&temp_id, set->table_size);
    
    HashSetNode *current = set->table[index];
    while (current) {
        if (current->id.st_dev == dev && current->id.st_ino == ino) {
            return current;
        }
        current = current->next;
    }
    return NULL;
}

/**
 * @brief [新增] DJB2 哈希算法 (简单高效字符串哈希)
 */
uint32_t calculate_name_hash(const char *str) {
    uint32_t hash = 5381;
    int c;
    while ((c = *str++))
        hash = ((hash << 5) + hash) + c; /* hash * 33 + c */
    return hash;
}
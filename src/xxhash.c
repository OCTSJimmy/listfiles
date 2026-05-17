/**
 * @file xxhash.c
 * @brief xxHash3 128-bit 哈希算法实现封装
 *
 * 本文件通过定义 XXH_IMPLEMENTATION 和 XXH_STATIC_LINKING_ONLY 宏，
 * 引入 xxhash.h 头文件以展开 xxHash3 的完整实现（含 128-bit 变种）。
 * 用于为文件路径、设备号(dev)、inode 号计算 128-bit 指纹，支撑去重与半增量扫描。
 *
 * @note 该文件仅包含宏定义与头文件包含，无独立可调用函数。
 * @see fingerprint_set.h 中的 fp_compute() 函数实际调用本文件提供的 XXH3_128bits 系列 API。
 */
#define XXH_IMPLEMENTATION
#define XXH_STATIC_LINKING_ONLY
#include "xxhash.h"

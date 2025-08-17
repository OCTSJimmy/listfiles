#ifndef UTILS_H
#define UTILS_H

#include <stddef.h>
#include "config.h" // 需要 Config 结构体来判断 verbose 等级

// 声明安全内存分配函数
void *safe_malloc(size_t size);

// 声明安全字符串拷贝函数
void safe_strcpy(char *dest, const char *src, size_t dest_size);

// 声明详细输出函数
void verbose_printf(const Config *cfg, int level, const char *format, ...);

// 声明时间格式化函数
const char *format_time(time_t t);

#endif // UTILS_H
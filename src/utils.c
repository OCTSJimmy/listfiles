#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

// verbose输出包装函数
void verbose_printf(const Config *cfg, int level, const char *format, ...) {
    if (!cfg->verbose) return;

    // 如果是按版本控制,且当前级别低于设置的级别,则不输出
    if (cfg->verbose_type == VERBOSE_TYPE_VERSIONED && level < cfg->verbose_level) {
        return;
    }

    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
}

const char *format_time(time_t t) {
    static __thread char buffer[32];  // 线程局部存储
    struct tm tm_buf;
    localtime_r(&t, &tm_buf);  // 线程安全版本
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tm_buf);
    return buffer;
}

void *safe_malloc(size_t size) {
    void *ptr = malloc(size);
    if (!ptr) {
        fprintf(stderr, "致命错误: 内存分配失败\n");
        exit(EXIT_FAILURE);
    }
    return ptr;
}

void safe_strcpy(char *dest, const char *src, size_t dest_size) {
    if (snprintf(dest, dest_size, "%s", src) >= (int)dest_size) {
        fprintf(stderr, "路径截断: %s\n", src);
        dest[dest_size - 1] = '\0';
    }
}
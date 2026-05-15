/**
 * @file utils.c
 * @brief 通用工具函数集合
 *
 * 提供安全内存分配、安全字符串拷贝、线程安全的时间格式化以及分级详细日志输出等基础工具。
 * 所有函数均为无状态设计，可在多线程环境下安全调用（format_time 使用 __thread 局部存储）。
 */
#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include "log.h"

/**
 * @brief  分级详细日志输出函数
 * @param  cfg    const Config*  指向全局配置结构体的指针，用于判断是否启用 verbose 及日志级别阈值
 * @param  level  int            当前日志消息的级别，取值范围: >= 0 的整数（越大越详细）
 * @param  format const char*    printf 风格的格式字符串
 * @param  ...                   可变参数列表，与 format 匹配
 * @return void
 *
 * @note   当 cfg->verbose 为 false 时直接返回，不做任何输出。
 *         当 cfg->verbose_type 为 VERBOSE_TYPE_VERSIONED 且 level < cfg->verbose_level 时也不输出。
 *         所有输出均定向到标准错误流(stderr)。
 */
void __attribute__((noinline)) verbose_printf(const Config *cfg, int level, const char *format, ...) {
    if (!cfg->verbose) return;

    if (cfg->verbose_type == VERBOSE_TYPE_VERSIONED && level < cfg->verbose_level) {
        return;
    }

    va_list args;
    va_start(args, format);
    log_vraw(format, args);
    va_end(args);
}

/**
 * @brief  将 time_t 时间戳格式化为可读字符串
 * @param  t  time_t  要格式化的 Unix 时间戳，取值范围: 有效 Unix 时间（通常为 1970 年之后）
 * @return const char*  指向格式化后字符串的指针，格式为 "YYYY-MM-DD HH:MM:SS"
 *
 * @warning 返回的指针指向线程局部存储(static __thread)缓冲区，无需释放，
 *          但同一线程后续调用会覆盖前一次结果。如需保留，调用方应自行拷贝。
 */
const char *format_time(time_t t) {
    static __thread char buffer[32];  // 线程局部存储，避免多线程竞争
    struct tm tm_buf;
    localtime_r(&t, &tm_buf);  // 线程安全版本
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &tm_buf);
    return buffer;
}

/**
 * @brief  安全内存分配函数（分配失败时直接退出进程）
 * @param  size  size_t  要分配的内存字节数，取值范围: > 0
 * @return void*  指向成功分配内存的指针，不会返回 NULL
 *
 * @note   若 malloc 失败，本函数会向 stderr 打印错误信息并调用 exit(EXIT_FAILURE) 终止程序。
 *         适用于对内存分配失败不可恢复的场景。
 */
void *safe_malloc(size_t size) {
    void *ptr = malloc(size);
    if (!ptr) {
        log_fatal("内存分配失败");
        exit(EXIT_FAILURE);
    }
    return ptr;
}

/**
 * @brief  安全字符串拷贝函数（自动处理截断并告警）
 * @param  dest       char*        目标缓冲区指针，不能为空
 * @param  src        const char*  源字符串指针，不能为空
 * @param  dest_size  size_t       目标缓冲区总容量（包含结尾的 '\0'），取值范围: > 0
 * @return void
 *
 * @note   使用 snprintf 进行拷贝。若源字符串长度超过 dest_size-1，
 *         会向 stderr 打印截断告警，并确保目标字符串以 '\0' 结尾。
 *         与 strncpy 不同，本函数总是生成合法的 C 字符串。
 */
void safe_strcpy(char *dest, const char *src, size_t dest_size) {
    if (snprintf(dest, dest_size, "%s", src) >= (int)dest_size) {
        log_error("路径截断: %s", src);
        dest[dest_size - 1] = '\0';
    }
}

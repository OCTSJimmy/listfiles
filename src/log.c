/**
 * @file log.c
 * @brief 统一日志实现 —— 时间戳 + 级别控制 + stderr 固定输出
 */
#define _GNU_SOURCE
#include "log.h"
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <string.h>

static bool g_verbose = false;
static int  g_verbose_level = 0;
static const char *level_names[] = {
    "FATAL", "ERROR", "WARN", "INFO", "DEBUG", "TRACE"
};

void log_init(bool enable_verbose, int verbose_level) {
    g_verbose = enable_verbose;
    g_verbose_level = verbose_level;
}

void log_set_verbose(bool enable, int level) {
    g_verbose = enable;
    g_verbose_level = level;
}

LogLevel log_get_threshold(void) {
    if (!g_verbose) return LOG_LEVEL_WARN;
    if (g_verbose_level >= 3) return LOG_LEVEL_TRACE;
    if (g_verbose_level >= 2) return LOG_LEVEL_DEBUG;
    if (g_verbose_level >= 1) return LOG_LEVEL_INFO;
    return LOG_LEVEL_INFO;
}

static void log_timestamp(char *buf, size_t size) {
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    strftime(buf, size, "%Y-%m-%d %H:%M:%S", tm_info);
}

void log_msg(LogLevel level, const char *fmt, ...) {
    LogLevel threshold = log_get_threshold();
    if (level > threshold) return;

    char ts[32];
    log_timestamp(ts, sizeof(ts));

    flockfile(stderr);

    fprintf(stderr, "[%s] [%s] ", ts, level_names[level]);

    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);

    /* 确保换行 */
    size_t len = strlen(fmt);
    if (len == 0 || fmt[len - 1] != '\n') {
        fprintf(stderr, "\n");
    }
    fflush(stderr);

    funlockfile(stderr);
}

void log_raw(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_vraw(fmt, args);
    va_end(args);
}

void __attribute__((noinline)) log_vraw(const char *fmt, va_list args) {
    char ts[32];
    log_timestamp(ts, sizeof(ts));

    flockfile(stderr);

    fprintf(stderr, "[%s] ", ts);

    va_list args_copy;
    va_copy(args_copy, args);
    vfprintf(stderr, fmt, args_copy);
    va_end(args_copy);

    size_t len = strlen(fmt);
    if (len == 0 || fmt[len - 1] != '\n') {
        fprintf(stderr, "\n");
    }
    fflush(stderr);

    funlockfile(stderr);
}

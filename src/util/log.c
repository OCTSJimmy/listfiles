/**
 * @file log.c
 * @brief 统一日志实现 —— 时间戳 + 级别控制 + stderr 固定输出
 */
#define _GNU_SOURCE
#include "log.h"
#include "config.h"
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <string.h>
#include <unistd.h>

static bool g_verbose = false;
static int  g_verbose_level = 0;
unsigned long g_log_version_threshold = VERSION_CODE;
/* v15.6.1（P0-107c）：fork 子进程无锁日志模式标志（worker_main 第一时间置位） */
static bool g_in_forked_child = false;
static const char *level_names[] = {
    "FATAL", "ERROR", "WARN", "INFO", "DEBUG", "TRACE"
};

static void log_timestamp(char *buf, size_t size);

void log_set_forked_child(void) {
    g_in_forked_child = true;
}

/* v15.6.1（P0-107c）：fork 子进程专用无锁输出——不碰 flockfile/stdio，
 * 栈缓冲组装后单次 write(2)。注意 localtime 的 tzset 锁仍是理论残留风险，
 * 但主要死锁向量（stderr 的 FILE 锁）已消除。 */
static void log_write_childsafe(LogLevel level, const char *fmt, va_list args) {
    char ts[32];
    log_timestamp(ts, sizeof(ts));

    char buf[2048];
    int off = snprintf(buf, sizeof(buf), "[%s] [%s] ", ts, level_names[level]);
    if (off < 0) return;
    if ((size_t)off >= sizeof(buf)) off = sizeof(buf) - 1;
    va_list args_copy;
    va_copy(args_copy, args);
    vsnprintf(buf + off, sizeof(buf) - (size_t)off, fmt, args_copy);
    va_end(args_copy);
    size_t len = strlen(buf);
    if (len > 0 && buf[len - 1] != '\n' && len < sizeof(buf) - 1) {
        buf[len++] = '\n';
        buf[len] = '\0';
    }
    (void)!write(STDERR_FILENO, buf, len);
}

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

void log_set_version_threshold(unsigned long threshold) {
    g_log_version_threshold = threshold;
}

unsigned long log_get_version_threshold(void) {
    return g_log_version_threshold;
}

static void log_timestamp(char *buf, size_t size) {
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    strftime(buf, size, "%Y-%m-%d %H:%M:%S", tm_info);
}

void log_msg(LogLevel level, const char *fmt, ...) {
    LogLevel threshold = log_get_threshold();
    if (level > threshold) return;

    /* v15.6.1（P0-107c）：fork 子进程走无锁路径，规避继承自多线程父进程的
     * stdio 锁死锁（持锁线程在子进程中不存在） */
    if (g_in_forked_child) {
        va_list args;
        va_start(args, fmt);
        log_write_childsafe(level, fmt, args);
        va_end(args);
        return;
    }

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
    /* v15.6.1（P0-107c）：fork 子进程走无锁路径（log_raw 无级别前缀，
     * 复用 childsafe 组装，级别按 WARN 展示） */
    if (g_in_forked_child) {
        log_write_childsafe(LOG_LEVEL_WARN, fmt, args);
        return;
    }

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

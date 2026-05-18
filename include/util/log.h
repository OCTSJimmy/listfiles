/**
 * @file log.h
 * @brief 统一日志模块 —— 时间戳 + 级别控制 + stderr 固定输出
 *
 * 所有日志统一通过本模块输出到 stderr，避免 stdout 污染。
 * 调试日志受 verbose / verbose_level 开关控制。
 */
#ifndef LOG_H
#define LOG_H

#include "config.h"
#include <stdbool.h>
#include <stdarg.h>

/* 日志级别 */
typedef enum {
    LOG_LEVEL_FATAL = 0,   /* 致命错误，始终输出 */
    LOG_LEVEL_ERROR = 1,   /* 错误，始终输出 */
    LOG_LEVEL_WARN  = 2,   /* 警告，始终输出 */
    LOG_LEVEL_INFO  = 3,   /* 信息，verbose 时输出 */
    LOG_LEVEL_DEBUG = 4,   /* 调试，verbose_level >= 2 时输出 */
    LOG_LEVEL_TRACE = 5,   /* 追踪，verbose_level >= 3 时输出 */
} LogLevel;

/**
 * @brief 初始化日志系统
 * @param enable_verbose  是否开启 verbose 模式
 * @param verbose_level   详细级别 (0-3)
 */
void log_init(bool enable_verbose, int verbose_level);

/**
 * @brief 设置当前 verbose 状态（运行时切换）
 */
void log_set_verbose(bool enable, int level);

/**
 * @brief 获取当前日志级别阈值
 */
LogLevel log_get_threshold(void);

/**
 * @brief 带时间戳的日志输出
 * @param level  日志级别
 * @param fmt    printf 格式字符串
 *
 * 输出格式: [YYYY-MM-DD HH:MM:SS] [LEVEL] message\n
 */
void log_msg(LogLevel level, const char *fmt, ...);

/* 版本化日志阈值 */
extern unsigned long g_log_version_threshold;

void log_set_version_threshold(unsigned long threshold);
unsigned long log_get_version_threshold(void);

/* 带版本的日志宏 */
#define log_fatal_v(ver, ...)  do { if ((unsigned long)(ver) >= g_log_version_threshold) log_msg(LOG_LEVEL_FATAL, __VA_ARGS__); } while(0)
#define log_error_v(ver, ...)  do { if ((unsigned long)(ver) >= g_log_version_threshold) log_msg(LOG_LEVEL_ERROR, __VA_ARGS__); } while(0)
#define log_warn_v(ver, ...)   do { if ((unsigned long)(ver) >= g_log_version_threshold) log_msg(LOG_LEVEL_WARN,  __VA_ARGS__); } while(0)
#define log_info_v(ver, ...)   do { if ((unsigned long)(ver) >= g_log_version_threshold) log_msg(LOG_LEVEL_INFO,  __VA_ARGS__); } while(0)
#define log_debug_v(ver, ...)  do { if ((unsigned long)(ver) >= g_log_version_threshold) log_msg(LOG_LEVEL_DEBUG, __VA_ARGS__); } while(0)
#define log_trace_v(ver, ...)  do { if ((unsigned long)(ver) >= g_log_version_threshold) log_msg(LOG_LEVEL_TRACE, __VA_ARGS__); } while(0)

/* 默认版本 = VERSION_CODE（不传 --verbose-version 时始终输出） */
#define log_fatal(...)  log_fatal_v(VERSION_CODE, __VA_ARGS__)
#define log_error(...)  log_error_v(VERSION_CODE, __VA_ARGS__)
#define log_warn(...)   log_warn_v(VERSION_CODE, __VA_ARGS__)
#define log_info(...)   log_info_v(VERSION_CODE, __VA_ARGS__)
#define log_debug(...)  log_debug_v(VERSION_CODE, __VA_ARGS__)
#define log_trace(...)  log_trace_v(VERSION_CODE, __VA_ARGS__)

/**
 * @brief 不带前缀的原始 stderr 输出（用于已有格式化内容）
 * @param fmt  printf 格式字符串
 *
 * 仅加时间戳，不加 [LEVEL]。用于兼容旧的中文错误消息等。
 */
void log_raw(const char *fmt, ...);

/**
 * @brief va_list 版本的 log_raw
 */
void log_vraw(const char *fmt, va_list args);

#endif /* LOG_H */

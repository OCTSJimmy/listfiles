#ifndef CIRCUIT_BREAKER_H
#define CIRCUIT_BREAKER_H

#include <sys/types.h>
#include <stdio.h>
#include <pthread.h>

struct AppContext;

/**
 * @brief 初始化熔断清单文件
 * @param ctx AppContext 指针
 * @note 打开 {progress_base}.circuit_breaker 用于追加写入
 */
void circuit_breaker_init(struct AppContext *ctx);

/**
 * @brief 记录一次熔断/跳过事件
 * @param ctx        AppContext 指针
 * @param reason     原因字符串，如 DEV_TIMEOUT/EIO/BLACKLIST/CONDEMNED
 * @param path       被跳过的路径
 * @param dev        设备号
 * @param retry_count 当前重试次数（无则传 0）
 * @note 线程安全，内部加锁并立即 fflush
 */
void circuit_breaker_record(struct AppContext *ctx, const char *reason,
                            const char *path, dev_t dev, int retry_count);

/**
 * @brief 关闭熔断清单文件
 * @param ctx AppContext 指针
 */
void circuit_breaker_close(struct AppContext *ctx);

#endif

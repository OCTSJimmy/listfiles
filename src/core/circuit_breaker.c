#define _GNU_SOURCE
#include "circuit_breaker.h"
#include "app_context.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

void circuit_breaker_init(struct AppContext *ctx) {
    if (!ctx || !ctx->cfg.progress_base) return;

    char path[1024];
    snprintf(path, sizeof(path), "%s.circuit_breaker", ctx->cfg.progress_base);

    /* 如果文件为空或不存在，后续写入表头 */
    struct stat st;
    bool need_header = (stat(path, &st) != 0 || st.st_size == 0);

    pthread_mutex_init(&ctx->circuit_breaker_mutex, NULL);
    ctx->circuit_breaker_fp = fopen(path, "a");
    if (!ctx->circuit_breaker_fp) {
        log_error("[CircuitBreaker] 无法打开清单文件: %s", path);
        return;
    }

    if (need_header) {
        fprintf(ctx->circuit_breaker_fp,
                "# timestamp\treason\tpath\tdev\tretry_count\n");
        fflush(ctx->circuit_breaker_fp);
    }
}

void circuit_breaker_record(struct AppContext *ctx, const char *reason,
                            const char *path, dev_t dev, int retry_count) {
    if (!ctx || !reason || !path) return;

    /* 原子累加跳过计数：即使清单文件无法写入，也必须让退出码非 0 */
    __atomic_fetch_add(&ctx->state.skipped_count, 1, __ATOMIC_RELAXED);

    if (!ctx->circuit_breaker_fp) return;

    pthread_mutex_lock(&ctx->circuit_breaker_mutex);

    time_t now = time(NULL);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);

    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_buf);

    fprintf(ctx->circuit_breaker_fp, "%s\t%s\t%s\t%lu\t%d\n",
            ts, reason, path, (unsigned long)dev, retry_count);
    fflush(ctx->circuit_breaker_fp);

    pthread_mutex_unlock(&ctx->circuit_breaker_mutex);
}

void circuit_breaker_close(struct AppContext *ctx) {
    if (!ctx) return;
    if (ctx->circuit_breaker_fp) {
        fclose(ctx->circuit_breaker_fp);
        ctx->circuit_breaker_fp = NULL;
    }
    pthread_mutex_destroy(&ctx->circuit_breaker_mutex);
}

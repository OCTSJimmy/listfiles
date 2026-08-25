/**
 * @file inject_lstat.c
 * @brief LD_PRELOAD 故障注入 shim：劫持 lstat()/stat() 对匹配路径返回指定 errno
 *
 * 复现生产环境 NFSv4 服务端 UTF-8 校验拒绝（NFS4ERR_INVAL）的场景：
 * 截断/损坏 UTF-8 文件名在 LOOKUP/GETATTR 时被服务端拒绝，客户端 lstat
 * 返回 EINVAL(22)；READDIR 不逐名校验，故 readdir 能列出但 stat 永远失败。
 * 本地文件系统不会主动产生该 errno，需注入模拟。
 *
 * 环境变量：
 *   LF_LSTAT_TARGET_SUBSTR   路径包含该子串时注入失败（必填，否则不注入）；
 *   LF_LSTAT_ERRNO           注入的 errno，默认 22（EINVAL）。
 *
 * 用法：
 *   gcc -shared -fPIC -o inject_lstat.so inject_lstat.c -ldl
 *   LF_LSTAT_TARGET_SUBSTR=badname LD_PRELOAD=./inject_lstat.so ./bin/listfiles -p ...
 *
 * 预期（listfiles v15.6.2+）：
 *   条目级：EINVAL 条目退化输出（名字进 CSV、元数据零字段）+ ENTRY_ERROR(errno=22)；
 *   目录级：RET_ERROR → SP_REASON_INVALID_NAME(6) 永久跳过，不触发设备探测，
 *           不发生"探测恒成功→假恢复→重入队"无限循环。
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef int (*lstat_fn)(const char *, struct stat *);
typedef int (*stat_fn)(const char *, struct stat *);
/* glibc < 2.33：二进制实际调用的是带版本号的 __xstat/__lxstat */
typedef int (*xstat_fn)(int, const char *, struct stat *);

static lstat_fn real_lstat;
static stat_fn  real_stat;
static xstat_fn real_xstat;
static xstat_fn real_lxstat;
static char     target_substr[4096];
static int      has_target;
static int      inject_errno = 22; /* EINVAL */

static void shim_init(void) {
    if (real_lstat) return;
    real_lstat  = (lstat_fn)dlsym(RTLD_NEXT, "lstat");
    real_stat   = (stat_fn)dlsym(RTLD_NEXT, "stat");
    real_xstat  = (xstat_fn)dlsym(RTLD_NEXT, "__xstat");
    real_lxstat = (xstat_fn)dlsym(RTLD_NEXT, "__lxstat");
    const char *t = getenv("LF_LSTAT_TARGET_SUBSTR");
    if (t && *t) {
        snprintf(target_substr, sizeof(target_substr), "%s", t);
        has_target = 1;
    }
    const char *e = getenv("LF_LSTAT_ERRNO");
    if (e && *e) inject_errno = atoi(e);
}

/* 返回 1 = 已注入失败（errno 已设置）；0 = 放行 */
static int maybe_inject(const char *path) {
    shim_init();
    if (!has_target || !path) return 0;
    if (strstr(path, target_substr)) {
        errno = inject_errno;
        return 1;
    }
    return 0;
}

int lstat(const char *path, struct stat *st) {
    if (maybe_inject(path)) return -1;
    return real_lstat(path, st);
}

int stat(const char *path, struct stat *st) {
    if (maybe_inject(path)) return -1;
    return real_stat(path, st);
}

int __xstat(int ver, const char *path, struct stat *st) {
    if (maybe_inject(path)) return -1;
    return real_xstat(ver, path, st);
}

int __lxstat(int ver, const char *path, struct stat *st) {
    if (maybe_inject(path)) return -1;
    return real_lxstat(ver, path, st);
}

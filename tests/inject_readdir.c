/**
 * @file inject_readdir.c
 * @brief LD_PRELOAD 故障注入 shim：劫持 readdir() 制造"无 errno 的假空/假 EOF"
 *
 * 复现 listfiles 故障总结 v2.0.0 §3.3 机制 A 的注入形态：
 * NFS 协议层静默截断——readdir 返回 NULL 但 errno=0，不产生任何错误码，
 * 传统 errno 检测族（v15.5.7 全部监控）对此整体失效。
 *
 * 环境变量：
 *   LF_TARGET_SUBSTR   仅对路径包含该子串的目录注入（opendir 路径匹配）；
 *                      未设置时对所有目录注入（通常会把整次扫描搞崩，仅调试用）。
 *   LF_FAKE_EMPTY_AFTER=N
 *                      目标 DIR 流返回第 N+1 个条目时（计数含 "." ".."），
 *                      返回 NULL 且 errno=0（假 EOF）。N=0 即首次调用即假空。
 *
 * 用法：
 *   gcc -shared -fPIC -o inject_readdir.so inject_readdir.c -ldl
 *   LF_TARGET_SUBSTR=inject_target LF_FAKE_EMPTY_AFTER=0 \
 *       LD_PRELOAD=./inject_readdir.so ./bin/listfiles -p ... 
 *
 * 预期（listfiles v15.5.8+）：
 *   --strict-nlink 下注入目标目录（含子目录）触发 NLINK_MISMATCH，
 *   记入熔断清单并以非零退出码结束；不加 --strict-nlink 则无感通过
 *   （纯文件目录的假空/截断客户端原理上不可检，见 fix_documents）。
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef DIR *(*opendir_fn)(const char *);
typedef struct dirent *(*readdir_fn)(DIR *);
typedef int (*closedir_fn)(DIR *);

static opendir_fn  real_opendir;
static readdir_fn  real_readdir;
static closedir_fn real_closedir;

static char  target_substr[4096];
static int   has_target;     /* 0 = 对所有目录注入 */
static long  fake_after = -1; /* <0 = 不注入 */

#define MAX_STREAMS 128
static struct { DIR *dp; int matched; long count; } streams[MAX_STREAMS];
static pthread_mutex_t streams_mu = PTHREAD_MUTEX_INITIALIZER;

static void shim_init(void) {
    if (real_opendir) return;
    real_opendir  = (opendir_fn)dlsym(RTLD_NEXT, "opendir");
    real_readdir  = (readdir_fn)dlsym(RTLD_NEXT, "readdir");
    real_closedir = (closedir_fn)dlsym(RTLD_NEXT, "closedir");
    const char *t = getenv("LF_TARGET_SUBSTR");
    if (t && *t) {
        snprintf(target_substr, sizeof(target_substr), "%s", t);
        has_target = 1;
    }
    const char *n = getenv("LF_FAKE_EMPTY_AFTER");
    if (n) fake_after = atol(n);
}

DIR *opendir(const char *name) {
    shim_init();
    DIR *dp = real_opendir(name);
    if (!dp || fake_after < 0) return dp;
    int matched = has_target ? (strstr(name, target_substr) != NULL) : 1;
    pthread_mutex_lock(&streams_mu);
    for (int i = 0; i < MAX_STREAMS; i++) {
        if (!streams[i].dp) {
            streams[i].dp = dp;
            streams[i].matched = matched;
            streams[i].count = 0;
            break;
        }
    }
    pthread_mutex_unlock(&streams_mu);
    return dp;
}

struct dirent *readdir(DIR *dirp) {
    shim_init();
    pthread_mutex_lock(&streams_mu);
    for (int i = 0; i < MAX_STREAMS; i++) {
        if (streams[i].dp == dirp) {
            if (streams[i].matched && streams[i].count >= fake_after) {
                pthread_mutex_unlock(&streams_mu);
                errno = 0; /* 关键：无 errno 的假 EOF */
                return NULL;
            }
            streams[i].count++;
            break;
        }
    }
    pthread_mutex_unlock(&streams_mu);
    return real_readdir(dirp);
}

int closedir(DIR *dirp) {
    shim_init();
    pthread_mutex_lock(&streams_mu);
    for (int i = 0; i < MAX_STREAMS; i++) {
        if (streams[i].dp == dirp) {
            streams[i].dp = NULL;
            break;
        }
    }
    pthread_mutex_unlock(&streams_mu);
    return real_closedir(dirp);
}

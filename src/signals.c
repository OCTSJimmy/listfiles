#include "signals.h"
#include "utils.h" // for safe_strcpy
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <stdio.h>
#include <pthread.h>

#define MAX_ACTIVE_LOCKS 10

typedef struct {
    int fd;
    char path[1024];
    bool is_main_lock;
} ActiveLockInfo;

// 将这些全局变量定义在这里，并设为 static，使其成为本文件的私有变量
static ActiveLockInfo g_active_locks[MAX_ACTIVE_LOCKS];
static int g_active_lock_count = 0;
static pthread_mutex_t g_lock_registry_mutex = PTHREAD_MUTEX_INITIALIZER;

// 注册一个已锁定的文件
void register_locked_file(int fd, const char* path, bool is_main) {
    pthread_mutex_lock(&g_lock_registry_mutex);
    if (g_active_lock_count < MAX_ACTIVE_LOCKS) {
        ActiveLockInfo* info = &g_active_locks[g_active_lock_count++];
        info->fd = fd;
        safe_strcpy(info->path, path, sizeof(info->path));
        info->is_main_lock = is_main;
    }
    pthread_mutex_unlock(&g_lock_registry_mutex);
}

// 注销一个文件锁
void unregister_locked_file(int fd) {
    pthread_mutex_lock(&g_lock_registry_mutex);
    int found_idx = -1;
    for (int i = 0; i < g_active_lock_count; i++) {
        if (g_active_locks[i].fd == fd) {
            found_idx = i;
            break;
        }
    }

    if (found_idx != -1) {
        // 将最后一个元素移动到当前位置，然后总数减一
        g_active_locks[found_idx] = g_active_locks[--g_active_lock_count];
    }
    pthread_mutex_unlock(&g_lock_registry_mutex);
}

// 终止信号处理器: SIGINT/SIGTERM/SIGQUIT
static void handle_terminate_signal(int sig) {
    const char msg[] = "收到终止信号，正在尝试释放锁并退出...\n";
    write(STDERR_FILENO, msg, sizeof(msg) - 1);

    for (int i = 0; i < g_active_lock_count; i++) {
        ActiveLockInfo* info = &g_active_locks[i];
        close(info->fd);
        if (info->is_main_lock) {
            unlink(info->path);
        }
    }

    signal(sig, SIG_DFL);
    raise(sig);
}

// 致命信号处理器: SIGSEGV/SIGABRT
static void handle_fatal_signal(int sig) {
    const char msg[] = "Fatal signal caught, aborting immediately.\n";
    write(STDERR_FILENO, msg, sizeof(msg) - 1);

    signal(sig, SIG_DFL);
    raise(sig);
}

// 在 main 函数开始时注册信号处理器
void setup_signal_handlers() {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART; // 自动重启被中断的系统调用

    // 致命信号: 不做清理，直接让内核生成 core dump
    sa.sa_handler = handle_fatal_signal;
    sigaction(SIGSEGV, &sa, NULL); // 段错误 (崩溃)
    sigaction(SIGABRT, &sa, NULL); // abort() 调用

    // 终止信号: 允许有限的清理
    sa.sa_handler = handle_terminate_signal;
    sigaction(SIGTERM, &sa, NULL); // 终止 (kill 命令)
    sigaction(SIGINT, &sa, NULL);  // 中断 (Ctrl+C)
    sigaction(SIGQUIT, &sa, NULL); // 退出 (Ctrl+\)
}

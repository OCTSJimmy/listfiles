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


// 信号处理器函数
void handle_fatal_signal(int sig) {
    // 信号处理器内部必须使用“异步信号安全”的函数
    // write 是安全的, printf/fprintf 不是
    const char msg[] = "收到致命信号，正在尝试释放锁并退出...\n";
    write(STDOUT_FILENO, msg, sizeof(msg) - 1);

    // 遍历全局注册表进行清理 (现在应该只包含主锁)
    for (int i = 0; i < g_active_lock_count; i++) {
        ActiveLockInfo* info = &g_active_locks[i];
        
        // fcntl 解锁逻辑已根据您的请求移除。
        
        close(info->fd);
        
        // 如果是主锁文件，则删除它
        if (info->is_main_lock) {
            unlink(info->path);
            const char main_lock_msg[] = "主锁文件已删除。\n";
            write(STDOUT_FILENO, main_lock_msg, sizeof(main_lock_msg) - 1);
        }
    }
    
    signal(sig, SIG_DFL);
    raise(sig);
}

// 在 main 函数开始时注册信号处理器
void setup_signal_handlers() {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_fatal_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART; // 自动重启被中断的系统调用

    // 为我们关心的信号注册处理器
    sigaction(SIGSEGV, &sa, NULL); // 段错误 (崩溃)
    sigaction(SIGTERM, &sa, NULL); // 终止 (kill 命令)
    sigaction(SIGINT, &sa, NULL);  // 中断 (Ctrl+C)
    sigaction(SIGQUIT, &sa, NULL); // 退出 (Ctrl+\)
    sigaction(SIGABRT, &sa, NULL); // abort() 调用
}

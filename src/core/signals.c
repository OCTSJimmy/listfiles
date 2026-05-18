/**
 * @file signals.c
 * @brief 信号处理模块
 *
 * 负责注册进程级信号处理器，实现优雅的终止清理与致命信号的快速退出。
 * 维护一个活跃文件锁注册表，在收到 SIGINT/SIGTERM/SIGQUIT 时自动释放已持有的文件锁，
 * 避免进度文件锁残留导致后续恢复失败。
 */
#include "signals.h"
#include "utils.h" // for safe_strcpy
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <stdio.h>
#include <pthread.h>

#define MAX_ACTIVE_LOCKS 10

/**
 * @brief 活跃文件锁信息结构体
 *
 * 记录当前进程持有的文件锁文件描述符、路径以及是否为全局主锁。
 * 主锁在信号处理时会被 unlink 删除，临时锁仅执行 close。
 */
typedef struct {
    int fd;
    char path[1024];
    bool is_main_lock;
} ActiveLockInfo;

// 将这些全局变量定义在这里，并设为 static，使其成为本文件的私有变量
static ActiveLockInfo g_active_locks[MAX_ACTIVE_LOCKS];
static int g_active_lock_count = 0;
static pthread_mutex_t g_lock_registry_mutex = PTHREAD_MUTEX_INITIALIZER;

/**
 * @brief  注册一个已锁定的文件到全局锁注册表
 * @param  fd       int         已持有 flock 的文件描述符，取值范围: >= 0 的有效 fd
 * @param  path     const char* 锁文件的路径字符串，不能为空
 * @param  is_main  bool        是否为主锁（主锁在信号处理时会被 unlink 删除）
 * @return void
 *
 * @note   本函数为线程安全实现，内部使用 g_lock_registry_mutex 保护。
 *         若注册表已满（达到 MAX_ACTIVE_LOCKS），则静默丢弃该记录。
 */
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

/**
 * @brief  从全局锁注册表中注销一个文件锁
 * @param  fd  int  要注销的文件描述符，取值范围: >= 0
 * @return void
 *
 * @note   本函数为线程安全实现。注销时采用"尾元素填充"策略保持数组紧凑。
 *         若 fd 未在注册表中找到，则不做任何操作。
 */
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

/**
 * @brief  终止信号处理器（SIGINT / SIGTERM / SIGQUIT）
 * @param  sig  int  接收到的信号编号，取值范围: SIGINT(2), SIGTERM(15), SIGQUIT(3)
 * @return void
 *
 * @note   使用 async-signal-safe 的 write 输出提示信息，
 *         遍历注册表 close 所有 fd，并对主锁执行 unlink 删除。
 *         清理完成后恢复信号默认行为并重新 raise 该信号，使进程正常终止。
 */
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

/**
 * @brief  致命信号处理器（SIGSEGV / SIGABRT）
 * @param  sig  int  接收到的信号编号，取值范围: SIGSEGV(11), SIGABRT(6)
 * @return void
 *
 * @note   致命信号发生时不做任何清理（避免在已损坏的堆栈上执行复杂逻辑），
 *         仅输出简短提示后立即恢复默认行为并重新 raise 信号，
 *         以便内核生成 core dump 或按默认策略终止进程。
 */
static void handle_fatal_signal(int sig) {
    const char msg[] = "Fatal signal caught, aborting immediately.\n";
    write(STDERR_FILENO, msg, sizeof(msg) - 1);

    signal(sig, SIG_DFL);
    raise(sig);
}

/**
 * @brief  在 main 函数开始时注册所有信号处理器
 * @return void
 *
 * @note   使用 sigaction 而非 signal() 以获得更可靠的行为。
 *         SA_RESTART 标志使被信号中断的系统调用自动重启。
 *         致命信号（SIGSEGV/SIGABRT）处理器不做清理；
 *         终止信号（SIGTERM/SIGINT/SIGQUIT）处理器执行有限清理后退出。
 */
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

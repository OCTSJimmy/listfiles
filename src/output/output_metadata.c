/**
 * @file output_metadata.c
 * @brief 元数据辅助函数
 *
 * 提供文件元数据查询与格式化辅助功能：
 * - 权限字符串格式化
 * - 设备状态缓存管理
 * - 扩展属性(lsattr)探测
 * - 用户名/组名缓存查询
 */
#include "output.h"
#include "utils.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pwd.h>
#include <grp.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <sys/stat.h>
#include "log.h"

/**
 * @brief  将 st_mode 转换为 ls -l 格式的权限字符串
 * @param  mode  mode_t  文件模式位，取值范围: 任意有效的 st_mode 值
 * @param  buf   char*   输出缓冲区，长度至少为 11 字节（10 个字符 + '\0'），不能为空
 * @return void
 *
 * @note   输出格式示例："-rw-r--r--"、"drwxr-xr-x"、"lrwxrwxrwx"。
 *         特殊位（SUID/SGID/Sticky）显示为 s/S、t/T。
 */
void format_mode_str(mode_t mode, char *buf) {
    strcpy(buf, "----------");
    const char chars[] = "rwxrwxrwx";
    if (S_ISREG(mode))  buf[0] = '-';
    else if (S_ISDIR(mode))  buf[0] = 'd';
    else if (S_ISLNK(mode))  buf[0] = 'l';
    else if (S_ISCHR(mode))  buf[0] = 'c';
    else if (S_ISBLK(mode))  buf[0] = 'b';
    else if (S_ISFIFO(mode)) buf[0] = 'p';
    else if (S_ISSOCK(mode)) buf[0] = 's';
    else buf[0] = '?';

    for (int i = 0; i < 9; i++) {
        buf[i + 1] = (mode & (1 << (8 - i))) ? chars[i] : '-';
    }
    if (mode & S_ISUID) buf[3] = (mode & S_IXUSR) ? 's' : 'S';
    if (mode & S_ISGID) buf[6] = (mode & S_IXGRP) ? 's' : 'S';
    if (mode & S_ISVTX) buf[9] = (mode & S_IXOTH) ? 't' : 'T';
    buf[10] = '\0';
}

/* =======================================================
 * lsattr 探测与缓存 (基于 RuntimeState)
 * ======================================================= */

/**
 * @brief  从设备缓存中查找指定设备的状态（线程安全）
 * @param  dev    dev_t          设备号
 * @param  state  RuntimeState*  运行时状态指针，不能为空
 * @return DeviceStatus  设备状态，取值范围: {DEV_STATUS_UNKNOWN, DEV_STATUS_SUPPORTED, DEV_STATUS_UNSUPPORTED}
 *
 * @note   内部加锁遍历 dev_cache 数组，时间复杂度 O(MAX_DEV_CACHE)。
 *         未找到时返回 DEV_STATUS_UNKNOWN。
 */
static DeviceStatus get_device_status(dev_t dev, RuntimeState *state) {
    DeviceStatus status = DEV_STATUS_UNKNOWN;
    
    pthread_mutex_lock(&state->dev_cache_mutex);
    for (size_t i = 0; i < state->dev_cache_count; i++) {
        if (state->dev_cache[i].dev == dev) {
            status = state->dev_cache[i].status;
            break;
        }
    }
    pthread_mutex_unlock(&state->dev_cache_mutex);
    
    return status;
}

/**
 * @brief  更新或新增设备状态到缓存（线程安全）
 * @param  dev     dev_t          设备号
 * @param  status  DeviceStatus   新的设备状态
 * @param  state   RuntimeState*  运行时状态指针，不能为空
 * @return void
 *
 * @note   采用双重检查模式：加锁后再次查找，若已存在则更新；
 *         若不存在且缓存未满，则追加新条目。
 *         缓存上限为 MAX_DEV_CACHE（64），超出时静默丢弃。
 */
static void set_device_status(dev_t dev, DeviceStatus status, RuntimeState *state) {
    pthread_mutex_lock(&state->dev_cache_mutex);
    
    // 双重检查：防止在等待锁的过程中已经被别的线程更新了
    for (size_t i = 0; i < state->dev_cache_count; i++) {
        if (state->dev_cache[i].dev == dev) {
            state->dev_cache[i].status = status;
            pthread_mutex_unlock(&state->dev_cache_mutex);
            return;
        }
    }

    if (state->dev_cache_count < MAX_DEV_CACHE) {
        state->dev_cache[state->dev_cache_count].dev = dev;
        state->dev_cache[state->dev_cache_count].status = status;
        state->dev_cache_count++;
    }
    
    pthread_mutex_unlock(&state->dev_cache_mutex);
}

/**
 * @brief  获取文件的 lsattr 扩展属性字符串
 * @param  state  RuntimeState*       运行时状态指针，不能为空
 * @param  path   const char*         文件路径，不能为空
 * @param  info   const struct stat*  文件 stat 信息指针，不能为空（用于获取设备号）
 * @param  buf    char*               输出缓冲区，长度至少为 16 字节，不能为空
 * @return void
 *
 * @note   流程：
 *         1. 检查设备缓存：若已知不支持则直接返回 "[unsupported]"
 *         2. 尝试 open(O_RDONLY | O_NONBLOCK) 获取 fd
 *         3. 尝试 ioctl(fd, FS_IOC_GETFLAGS, &flags)
 *            - 成功：解析 flags 为 16 字符属性串（如 "-------------e--"）
 *            - 失败且 errno 为 ENOTTY/EOPNOTSUPP：标记设备为不支持
 *            - 其他错误：返回 "[ioctl_error]"
 *         4. 关闭 fd
 *         该操作涉及系统调用，性能开销较大，仅在用户指定 --xattr 或格式串含 %%X 时执行。
 */
void get_xattr_str(RuntimeState *state, const char *path, const struct stat *info, char *buf) {
    // 1. 检查设备缓存
    DeviceStatus ds = get_device_status(info->st_dev, state);
    if (ds == DEV_STATUS_UNSUPPORTED) {
        strcpy(buf, "[unsupported]   ");
        return;
    }

    // 2. 尝试打开
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd == -1) {
        strcpy(buf, "[access_denied] "); 
        return;
    }

    // 3. 尝试 ioctl
    int flags = 0;
    if (ioctl(fd, FS_IOC_GETFLAGS, &flags) == 0) {
        // 成功！说明设备支持。如果之前是未知状态，更新为支持
        if (ds == DEV_STATUS_UNKNOWN) {
            set_device_status(info->st_dev, DEV_STATUS_SUPPORTED, state);
        }
        
        // 解析标志位
        strcpy(buf, "----------------");
        if (flags & FS_SECRM_FL)        buf[0] = 's';
        if (flags & FS_UNRM_FL)         buf[1] = 'u';
        if (flags & FS_COMPR_FL)        buf[2] = 'c';
        if (flags & FS_SYNC_FL)         buf[3] = 'S';
        if (flags & FS_IMMUTABLE_FL)    buf[4] = 'i';
        if (flags & FS_APPEND_FL)       buf[5] = 'a';
        if (flags & FS_NODUMP_FL)       buf[6] = 'd';
        if (flags & FS_NOATIME_FL)      buf[7] = 'A';
        if (flags & FS_DIRTY_FL)        buf[8] = 'D';
        if (flags & FS_COMPRBLK_FL)     buf[9] = 'B';
        if (flags & FS_NOCOMP_FL)       buf[10] = 'Z';
        #ifdef FS_ECOMPR_FL
        if (flags & FS_ECOMPR_FL)       buf[11] = 'E';
        #endif
        if (flags & FS_INDEX_FL)        buf[12] = 'I';
        if (flags & FS_IMAGIC_FL)       buf[13] = 'i';
        if (flags & FS_JOURNAL_DATA_FL) buf[14] = 'j';
        if (flags & FS_NOTAIL_FL)       buf[15] = 't';
    } else {
        // ioctl 失败
        if (errno == ENOTTY || errno == EOPNOTSUPP) {
            // 明确不支持，更新缓存
            set_device_status(info->st_dev, DEV_STATUS_UNSUPPORTED, state);
            strcpy(buf, "[unsupported]   ");
        } else {
            strcpy(buf, "[ioctl_error]   ");
        }
    }
    close(fd);
}

/**
 * @brief  获取用户名（含 UID 缓存哈希表）
 * @param  state  RuntimeState*  运行时状态指针，不能为空
 * @param  uid    uid_t          用户 ID，取值范围: 任意有效 Linux UID
 * @return const char*  格式化后的用户名字符串，如 "root(0)" 或 "1005"
 *
 * @note   使用哈希链表缓存（桶数 UID_CACHE_SIZE=4096），缓存命中时 O(1) 返回。
 *         缓存未命中时调用 getpwuid 查询系统，格式化为 "name(uid)" 或纯数字 UID。
 *         返回的指针指向缓存节点，无需释放，但生命周期与 RuntimeState 一致。
 */
const char *get_username(RuntimeState *state, uid_t uid) {
    // 计算哈希桶索引
    unsigned bucket = uid % UID_CACHE_SIZE;
    
    // 在桶中查找
    UserCacheEntry *entry = state->uid_cache[bucket];
    while (entry) {
        if (entry->uid == uid) {
            return entry->name;
        }
        entry = entry->next;
    }
    
    // 缓存未命中,查询系统
    struct passwd *pw = getpwuid(uid);
    char temp_buf[256]; // 临时缓冲区，足够容纳 name(uid)

    if (pw) {
        // 找到了：格式化为 "root(0)" 或 "www-data(33)"
        snprintf(temp_buf, sizeof(temp_buf), "%s(%u)", pw->pw_name, (unsigned int)uid);
    } else {
        // 没找到：直接保留原始 UID，例如 "1005"
        snprintf(temp_buf, sizeof(temp_buf), "%u", (unsigned int)uid);
    }
    
    // 创建新缓存项
    UserCacheEntry *new_entry = safe_malloc(sizeof(UserCacheEntry));
    new_entry->uid = uid;
    new_entry->name = safe_malloc(strlen(temp_buf) + 1);
    strcpy(new_entry->name, temp_buf);
    
    // 添加到桶的头部
    new_entry->next = state->uid_cache[bucket];
    state->uid_cache[bucket] = new_entry;
    state->uid_cache_count++;
    
    return new_entry->name;
}

/**
 * @brief  获取组名（含 GID 缓存哈希表）
 * @param  state  RuntimeState*  运行时状态指针，不能为空
 * @param  gid    gid_t          组 ID，取值范围: 任意有效 Linux GID
 * @return const char*  格式化后的组名字符串，如 "root(0)" 或 "1005"
 *
 * @note   逻辑与 get_username 完全相同，使用 GID_CACHE_SIZE=4096 的哈希链表缓存。
 *         缓存未命中时调用 getgrgid 查询系统。
 */
const char *get_groupname(RuntimeState *state, gid_t gid) {
    // 计算哈希桶索引
    unsigned bucket = gid % GID_CACHE_SIZE;
    
    // 在桶中查找
    GroupCacheEntry *entry = state->gid_cache[bucket];
    while (entry) {
        if (entry->gid == gid) {
            return entry->name;
        }
        entry = entry->next;
    }
    
    // 缓存未命中,查询系统
    struct group *gr = getgrgid(gid);
    char temp_buf[256];
    if (gr) {
        // 找到了：格式化为 "root(0)"
        snprintf(temp_buf, sizeof(temp_buf), "%s(%u)", gr->gr_name, (unsigned int)gid);
    } else {
        // 没找到：保留原始 GID，例如 "1005"
        snprintf(temp_buf, sizeof(temp_buf), "%u", (unsigned int)gid);
    }
    
    // 创建新缓存项
    GroupCacheEntry *new_entry = safe_malloc(sizeof(GroupCacheEntry));
    new_entry->gid = gid;
    new_entry->name = safe_malloc(strlen(temp_buf) + 1);
    strcpy(new_entry->name, temp_buf);
    
    // 添加到桶的头部
    new_entry->next = state->gid_cache[bucket];
    state->gid_cache[bucket] = new_entry;
    state->gid_cache_count++;
    
    return new_entry->name;
}

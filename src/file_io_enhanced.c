/**
 * 增强版文件I/O模块实现
 * 提供零拷贝、异步I/O、文件缓存等高性能文件处理功能
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <stdatomic.h>

#if defined(__linux__)
#include <sys/sendfile.h>
#elif defined(__APPLE__)
#include <sys/uio.h>
#endif

#include "../include/file_io_enhanced.h"
#include "../include/logger.h"

// Global variables
static file_cache_manager_t *g_cache_manager = NULL;
static file_io_stats_t g_stats = {0};
static file_io_config_t g_config = {0};
static atomic_int g_initialized = 0;

// 哈希函数
static size_t hash_string(const char *str) {
    size_t hash = 5381;
    int c;
    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c;
    }
    return hash;
}

// 获取当前时间戳（纳秒）
static uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

// 缓存清理线程
static void *cache_cleanup_thread(void *arg) {
    file_cache_manager_t *manager = (file_cache_manager_t *)arg;
    time_t now;
    file_cache_item_t *item, *prev, *next;
    
    while (!atomic_load(&manager->stop_cleanup)) {
        pthread_mutex_lock(&manager->mutex);
        
        now = time(NULL);
        
        // 遍历所有桶
        for (size_t i = 0; i < manager->bucket_count; i++) {
            prev = NULL;
            item = manager->buckets[i];
            
            while (item != NULL) {
                next = item->next;
                
                // 检查是否过期（超过1小时未访问）
                if (now - item->access_time > 3600) {
                    // 移除过期项
                    if (prev == NULL) {
                        manager->buckets[i] = next;
                    } else {
                        prev->next = next;
                    }
                    
                    // 释放内存
                    free(item->path);
                    free(item->data);
                    manager->current_size -= item->size;
                    free(item);
                } else {
                    prev = item;
                }
                
                item = next;
            }
        }
        
        pthread_mutex_unlock(&manager->mutex);
        
        // 等待清理间隔
        sleep(g_config.cache_cleanup_interval);
    }
    
    return NULL;
}

// 初始化文件I/O模块
int file_io_enhanced_init(const file_io_config_t *config) {
    if (atomic_load(&g_initialized)) {
        return 0; // 已经初始化
    }
    
    if (!config) {
        return -1;
    }
    
    // 复制配置
    memcpy(&g_config, config, sizeof(file_io_config_t));
    
    // 设置默认值
    if (g_config.cache_size == 0) g_config.cache_size = 100; // 100MB
    if (g_config.max_file_size == 0) g_config.max_file_size = 50; // 50MB
    if (g_config.read_buffer_size == 0) g_config.read_buffer_size = 8192;
    if (g_config.write_buffer_size == 0) g_config.write_buffer_size = 8192;
    if (g_config.cache_cleanup_interval == 0) g_config.cache_cleanup_interval = 300; // 5分钟
    
    // 初始化缓存管理器
    g_cache_manager = malloc(sizeof(file_cache_manager_t));
    if (!g_cache_manager) {
        return -1;
    }
    
    g_cache_manager->bucket_count = 1024; // 1024个桶
    g_cache_manager->max_size = g_config.cache_size * 1024 * 1024; // 转换为字节
    g_cache_manager->current_size = 0;
    
    g_cache_manager->buckets = calloc(g_cache_manager->bucket_count, sizeof(file_cache_item_t *));
    if (!g_cache_manager->buckets) {
        free(g_cache_manager);
        return -1;
    }
    
    pthread_mutex_init(&g_cache_manager->mutex, NULL);
    atomic_store(&g_cache_manager->stop_cleanup, 0);
    
    // 启动清理线程
    if (pthread_create(&g_cache_manager->cleanup_thread, NULL, cache_cleanup_thread, g_cache_manager) != 0) {
        pthread_mutex_destroy(&g_cache_manager->mutex);
        free(g_cache_manager->buckets);
        free(g_cache_manager);
        return -1;
    }
    
    // 重置统计信息
    memset(&g_stats, 0, sizeof(g_stats));
    
    atomic_store(&g_initialized, 1);
    
    log_info("增强版文件I/O模块初始化完成");
    return 0;
}

// 销毁文件I/O模块
void file_io_enhanced_destroy(void) {
    if (!atomic_load(&g_initialized)) {
        return;
    }
    
    if (g_cache_manager) {
        // 停止清理线程
        atomic_store(&g_cache_manager->stop_cleanup, 1);
        pthread_join(g_cache_manager->cleanup_thread, NULL);
        
        // 清空缓存
        file_io_enhanced_clear_cache();
        
        // 销毁互斥锁
        pthread_mutex_destroy(&g_cache_manager->mutex);
        
        // 释放内存
        free(g_cache_manager->buckets);
        free(g_cache_manager);
        g_cache_manager = NULL;
    }
    
    atomic_store(&g_initialized, 0);
    log_info("增强版文件I/O模块已销毁");
}

// 获取文件I/O统计信息
void file_io_enhanced_get_stats(file_io_stats_t *stats) {
    if (stats) {
        memcpy(stats, &g_stats, sizeof(file_io_stats_t));
    }
}

// 重置文件I/O统计信息
void file_io_enhanced_reset_stats(void) {
    memset(&g_stats, 0, sizeof(g_stats));
}

// 打印文件I/O统计信息
void file_io_enhanced_print_stats(void) {
    size_t current_size, max_size, hit_count, miss_count;
    file_io_enhanced_get_cache_info(&current_size, &max_size, &hit_count, &miss_count);
    
    log_info("=== 文件I/O统计信息 ===");
    log_info("总请求数: %lu", atomic_load(&g_stats.total_requests));
    log_info("缓存命中数: %lu", atomic_load(&g_stats.cache_hits));
    log_info("缓存未命中数: %lu", atomic_load(&g_stats.cache_misses));
    log_info("sendfile请求数: %lu", atomic_load(&g_stats.sendfile_requests));
    log_info("mmap请求数: %lu", atomic_load(&g_stats.mmap_requests));
    log_info("异步请求数: %lu", atomic_load(&g_stats.async_requests));
    log_info("总发送字节数: %lu", atomic_load(&g_stats.total_bytes_sent));
    log_info("总读取时间: %lu ns", atomic_load(&g_stats.total_read_time));
    log_info("总发送时间: %lu ns", atomic_load(&g_stats.total_send_time));
    log_info("缓存使用: %zu/%zu bytes (%.1f%%)", 
             current_size, max_size, (double)current_size / max_size * 100);
    log_info("缓存命中率: %.1f%%", 
             (hit_count + miss_count) > 0 ? (double)hit_count / (hit_count + miss_count) * 100 : 0);
}

// 从缓存获取文件
void *file_io_enhanced_get_from_cache(const char *file_path, size_t *size) {
    if (!g_cache_manager || !file_path) {
        return NULL;
    }
    
    size_t hash = hash_string(file_path) % g_cache_manager->bucket_count;
    
    pthread_mutex_lock(&g_cache_manager->mutex);
    
    file_cache_item_t *item = g_cache_manager->buckets[hash];
    while (item != NULL) {
        if (strcmp(item->path, file_path) == 0 && atomic_load(&item->is_valid)) {
            // 更新访问时间
            item->access_time = time(NULL);
            atomic_fetch_add(&item->ref_count, 1);
            
            if (size) *size = item->size;
            
            pthread_mutex_unlock(&g_cache_manager->mutex);
            atomic_fetch_add(&g_stats.cache_hits, 1);
            return item->data;
        }
        item = item->next;
    }
    
    pthread_mutex_unlock(&g_cache_manager->mutex);
    atomic_fetch_add(&g_stats.cache_misses, 1);
    return NULL;
}

// 将文件添加到缓存
int file_io_enhanced_add_to_cache(const char *file_path, const void *data, size_t size) {
    if (!g_cache_manager || !file_path || !data) {
        return -1;
    }
    
    // 检查文件大小是否超过限制
    if (size > (size_t)(g_config.max_file_size * 1024 * 1024)) {
        return -1;
    }
    
    size_t hash = hash_string(file_path) % g_cache_manager->bucket_count;
    
    pthread_mutex_lock(&g_cache_manager->mutex);
    
    // 检查是否已存在
    file_cache_item_t *item = g_cache_manager->buckets[hash];
    while (item != NULL) {
        if (strcmp(item->path, file_path) == 0) {
            // 更新现有项
            free(item->data);
            item->data = malloc(size);
            if (!item->data) {
                pthread_mutex_unlock(&g_cache_manager->mutex);
                return -1;
            }
            memcpy(item->data, data, size);
            item->size = size;
            item->mtime = time(NULL);
            item->access_time = time(NULL);
            atomic_store(&item->is_valid, 1);
            
            pthread_mutex_unlock(&g_cache_manager->mutex);
            return 0;
        }
        item = item->next;
    }
    
    // 创建新项
    item = malloc(sizeof(file_cache_item_t));
    if (!item) {
        pthread_mutex_unlock(&g_cache_manager->mutex);
        return -1;
    }
    
    item->path = strdup(file_path);
    item->data = malloc(size);
    if (!item->path || !item->data) {
        free(item->path);
        free(item->data);
        free(item);
        pthread_mutex_unlock(&g_cache_manager->mutex);
        return -1;
    }
    
    memcpy(item->data, data, size);
    item->size = size;
    item->mtime = time(NULL);
    item->access_time = time(NULL);
    atomic_store(&item->ref_count, 1);
    atomic_store(&item->is_valid, 1);
    
    // 添加到链表头部
    item->next = g_cache_manager->buckets[hash];
    g_cache_manager->buckets[hash] = item;
    g_cache_manager->current_size += size;
    
    pthread_mutex_unlock(&g_cache_manager->mutex);
    return 0;
}

// 从缓存移除文件
void file_io_enhanced_remove_from_cache(const char *file_path) {
    if (!g_cache_manager || !file_path) {
        return;
    }
    
    size_t hash = hash_string(file_path) % g_cache_manager->bucket_count;
    
    pthread_mutex_lock(&g_cache_manager->mutex);
    
    file_cache_item_t *item = g_cache_manager->buckets[hash];
    file_cache_item_t *prev = NULL;
    
    while (item != NULL) {
        if (strcmp(item->path, file_path) == 0) {
            if (prev == NULL) {
                g_cache_manager->buckets[hash] = item->next;
            } else {
                prev->next = item->next;
            }
            
            g_cache_manager->current_size -= item->size;
            free(item->path);
            free(item->data);
            free(item);
            break;
        }
        prev = item;
        item = item->next;
    }
    
    pthread_mutex_unlock(&g_cache_manager->mutex);
}

// 清空缓存
void file_io_enhanced_clear_cache(void) {
    if (!g_cache_manager) {
        return;
    }
    
    pthread_mutex_lock(&g_cache_manager->mutex);
    
    for (size_t i = 0; i < g_cache_manager->bucket_count; i++) {
        file_cache_item_t *item = g_cache_manager->buckets[i];
        while (item != NULL) {
            file_cache_item_t *next = item->next;
            free(item->path);
            free(item->data);
            free(item);
            item = next;
        }
        g_cache_manager->buckets[i] = NULL;
    }
    
    g_cache_manager->current_size = 0;
    
    pthread_mutex_unlock(&g_cache_manager->mutex);
}

// 使用sendfile发送文件
int file_io_enhanced_send_file_sendfile(int client_fd, const char *file_path, size_t *sent_bytes) {
    if (!g_config.enable_sendfile) {
        return -1;
    }
    
    uint64_t start_time = get_time_ns();
    
    int file_fd = open(file_path, O_RDONLY);
    if (file_fd < 0) {
        return -1;
    }
    
    struct stat st;
    if (fstat(file_fd, &st) < 0) {
        close(file_fd);
        return -1;
    }
    
    size_t total_sent = 0;
    off_t offset = 0;
    
#if defined(__linux__)
    // Linux sendfile
    while (total_sent < (size_t)st.st_size) {
        n = sendfile(client_fd, file_fd, &offset, st.st_size - total_sent);
        if (n > 0) {
            total_sent += n;
        } else if (n == 0) {
            break;
        } else {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(1000);
                continue;
            }
            close(file_fd);
            return -1;
        }
    }
#elif defined(__APPLE__)
    // macOS sendfile
    off_t len = st.st_size;
    int result = sendfile(file_fd, client_fd, offset, &len, NULL, 0);
    if (result == 0) {
        total_sent = len;
    } else {
        close(file_fd);
        return -1;
    }
#else
    close(file_fd);
    return -1;
#endif
    
    close(file_fd);
    
    if (sent_bytes) *sent_bytes = total_sent;
    
    atomic_fetch_add(&g_stats.sendfile_requests, 1);
    atomic_fetch_add(&g_stats.total_bytes_sent, total_sent);
    atomic_fetch_add(&g_stats.total_send_time, get_time_ns() - start_time);
    
    return 0;
}

// 使用mmap发送文件
int file_io_enhanced_send_file_mmap(int client_fd, const char *file_path, size_t *sent_bytes) {
    if (!g_config.enable_mmap) {
        return -1;
    }
    
    uint64_t start_time = get_time_ns();
    
    int file_fd = open(file_path, O_RDONLY);
    if (file_fd < 0) {
        return -1;
    }
    
    struct stat st;
    if (fstat(file_fd, &st) < 0) {
        close(file_fd);
        return -1;
    }
    
    // 使用mmap映射文件
    void *addr = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, file_fd, 0);
    if (addr == MAP_FAILED) {
        close(file_fd);
        return -1;
    }
    
    // 发送文件内容
    size_t total_sent = 0;
    ssize_t bytes_sent;
    
    while (total_sent < (size_t)st.st_size) {
        bytes_sent = write(client_fd, (char *)addr + total_sent, st.st_size - total_sent);
        if (bytes_sent > 0) {
            total_sent += bytes_sent;
        } else if (bytes_sent == 0) {
            break;
        } else {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(1000);
                continue;
            }
            munmap(addr, st.st_size);
            close(file_fd);
            return -1;
        }
    }
    
    munmap(addr, st.st_size);
    close(file_fd);
    
    if (sent_bytes) *sent_bytes = total_sent;
    
    atomic_fetch_add(&g_stats.mmap_requests, 1);
    atomic_fetch_add(&g_stats.total_bytes_sent, total_sent);
    atomic_fetch_add(&g_stats.total_send_time, get_time_ns() - start_time);
    
    return 0;
}

// 零拷贝文件发送（自动选择最优方式）
int file_io_enhanced_send_file(int client_fd, const char *file_path, size_t *sent_bytes) {
    if (!atomic_load(&g_initialized)) {
        return -1;
    }
    
    atomic_fetch_add(&g_stats.total_requests, 1);
    
    // 首先尝试从缓存获取
    size_t cached_size;
    void *cached_data = file_io_enhanced_get_from_cache(file_path, &cached_size);
    if (cached_data) {
        // 从缓存发送
        uint64_t start_time = get_time_ns();
        
        size_t total_sent = 0;
        ssize_t bytes_sent;
        
        while (total_sent < cached_size) {
            bytes_sent = write(client_fd, (char *)cached_data + total_sent, cached_size - total_sent);
            if (bytes_sent > 0) {
                total_sent += bytes_sent;
            } else if (bytes_sent == 0) {
                break;
            } else {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    usleep(1000);
                    continue;
                }
                return -1;
            }
        }
        
        if (sent_bytes) *sent_bytes = total_sent;
        atomic_fetch_add(&g_stats.total_bytes_sent, total_sent);
        atomic_fetch_add(&g_stats.total_send_time, get_time_ns() - start_time);
        
        return 0;
    }
    
    // 获取文件信息
    struct stat st;
    if (stat(file_path, &st) < 0) {
        return -1;
    }
    
    // 根据文件大小选择最优方式
    if (st.st_size <= 1024 * 1024) { // 小于1MB，使用sendfile
        return file_io_enhanced_send_file_sendfile(client_fd, file_path, sent_bytes);
    } else { // 大于1MB，使用mmap
        return file_io_enhanced_send_file_mmap(client_fd, file_path, sent_bytes);
    }
}

// 预加载文件到缓存
int file_io_enhanced_preload_file(const char *file_path) {
    if (!atomic_load(&g_initialized)) {
        return -1;
    }
    
    // 检查是否已在缓存中
    if (file_io_enhanced_is_cached(file_path)) {
        return 0;
    }
    
    int file_fd = open(file_path, O_RDONLY);
    if (file_fd < 0) {
        return -1;
    }
    
    struct stat st;
    if (fstat(file_fd, &st) < 0) {
        close(file_fd);
        return -1;
    }
    
    // 检查文件大小
    if (st.st_size > (off_t)(g_config.max_file_size * 1024 * 1024)) {
        close(file_fd);
        return -1;
    }
    
    // 读取文件内容
    void *data = malloc(st.st_size);
    if (!data) {
        close(file_fd);
        return -1;
    }
    
    ssize_t bytes_read = read(file_fd, data, st.st_size);
    close(file_fd);
    
    if (bytes_read != st.st_size) {
        free(data);
        return -1;
    }
    
    // 添加到缓存
    int result = file_io_enhanced_add_to_cache(file_path, data, st.st_size);
    free(data);
    
    return result;
}

// 批量预加载文件
int file_io_enhanced_preload_files(const char **file_paths, int count) {
    if (!file_paths || count <= 0) {
        return -1;
    }
    
    int success_count = 0;
    for (int i = 0; i < count; i++) {
        if (file_io_enhanced_preload_file(file_paths[i]) == 0) {
            success_count++;
        }
    }
    
    return success_count;
}

// 检查文件是否在缓存中
int file_io_enhanced_is_cached(const char *file_path) {
    if (!g_cache_manager || !file_path) {
        return 0;
    }
    
    size_t hash = hash_string(file_path) % g_cache_manager->bucket_count;
    
    pthread_mutex_lock(&g_cache_manager->mutex);
    
    file_cache_item_t *item = g_cache_manager->buckets[hash];
    while (item != NULL) {
        if (strcmp(item->path, file_path) == 0 && atomic_load(&item->is_valid)) {
            pthread_mutex_unlock(&g_cache_manager->mutex);
            return 1;
        }
        item = item->next;
    }
    
    pthread_mutex_unlock(&g_cache_manager->mutex);
    return 0;
}

// 获取缓存使用情况
void file_io_enhanced_get_cache_info(size_t *current_size, size_t *max_size, 
                                    size_t *hit_count, size_t *miss_count) {
    if (current_size) *current_size = g_cache_manager ? g_cache_manager->current_size : 0;
    if (max_size) *max_size = g_cache_manager ? g_cache_manager->max_size : 0;
    if (hit_count) *hit_count = atomic_load(&g_stats.cache_hits);
    if (miss_count) *miss_count = atomic_load(&g_stats.cache_misses);
}

// 获取文件信息（缓存友好）
int file_io_enhanced_get_file_info(const char *file_path, struct stat *st) {
    if (!file_path || !st) {
        return -1;
    }
    
    return stat(file_path, st);
} 
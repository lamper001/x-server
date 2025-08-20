/**
 * 统一资源管理模块 - 优化内存、文件描述符和连接资源的管理
 */

#ifndef RESOURCE_MANAGER_H
#define RESOURCE_MANAGER_H

#include <stdint.h>
#include <stddef.h>
#include <pthread.h>

// 资源类型定义
typedef enum {
    RESOURCE_MEMORY,        // 内存资源
    RESOURCE_FILE_DESC,     // 文件描述符
    RESOURCE_CONNECTION,    // 连接资源
    RESOURCE_THREAD,        // 线程资源
    RESOURCE_CACHE          // 缓存资源
} resource_type_t;

// 资源状态
typedef enum {
    RESOURCE_STATUS_AVAILABLE,  // 可用
    RESOURCE_STATUS_IN_USE,     // 使用中
    RESOURCE_STATUS_RESERVED,   // 已预留
    RESOURCE_STATUS_ERROR       // 错误状态
} resource_status_t;

// 资源限制配置
typedef struct {
    size_t max_memory_usage;        // 最大内存使用量(字节)
    int max_file_descriptors;       // 最大文件描述符数
    int max_connections;            // 最大连接数
    int max_threads;                // 最大线程数
    size_t max_cache_size;          // 最大缓存大小
    
    // 清理策略
    int enable_auto_cleanup;        // 启用自动清理
    int cleanup_interval;           // 清理间隔(秒)
    double cleanup_threshold;       // 清理阈值(使用率)
    
    // 监控配置
    int enable_monitoring;          // 启用资源监控
    int alert_threshold;            // 告警阈值(百分比)
} resource_config_t;

// 资源使用统计
typedef struct {
    // 内存统计
    size_t memory_allocated;        // 已分配内存
    size_t memory_peak;             // 内存使用峰值
    uint64_t memory_alloc_count;    // 内存分配次数
    uint64_t memory_free_count;     // 内存释放次数
    
    // 文件描述符统计
    int fd_in_use;                  // 使用中的文件描述符
    int fd_peak;                    // 文件描述符使用峰值
    uint64_t fd_open_count;         // 打开次数
    uint64_t fd_close_count;        // 关闭次数
    
    // 连接统计
    int connections_active;         // 活跃连接数
    int connections_peak;           // 连接数峰值
    uint64_t connections_total;     // 总连接数
    uint64_t connections_closed;    // 已关闭连接数
    
    // 线程统计
    int threads_active;             // 活跃线程数
    int threads_peak;               // 线程数峰值
    uint64_t threads_created;       // 创建的线程数
    uint64_t threads_destroyed;     // 销毁的线程数
    
    // 缓存统计
    size_t cache_used;              // 已使用缓存
    size_t cache_peak;              // 缓存使用峰值
    uint64_t cache_hits;            // 缓存命中数
    uint64_t cache_misses;          // 缓存未命中数
    
    // 清理统计
    uint64_t cleanup_runs;          // 清理运行次数
    uint64_t resources_cleaned;     // 清理的资源数
    
    time_t last_cleanup;            // 最后清理时间
    time_t start_time;              // 统计开始时间
} resource_stats_t;

// 资源告警回调函数类型
typedef void (*resource_alert_callback_t)(resource_type_t type, double usage_percent, const char *message);

// 函数声明

/**
 * 初始化资源管理器
 */
int resource_manager_init(const resource_config_t *config);

/**
 * 销毁资源管理器
 */
void resource_manager_destroy(void);

/**
 * 分配内存资源
 */
void *resource_malloc(size_t size);

/**
 * 释放内存资源
 */
void resource_free(void *ptr);

/**
 * 重新分配内存资源
 */
void *resource_realloc(void *ptr, size_t new_size);

/**
 * 注册文件描述符
 */
int resource_register_fd(int fd);

/**
 * 注销文件描述符
 */
void resource_unregister_fd(int fd);

/**
 * 注册连接
 */
int resource_register_connection(void *conn_ptr);

/**
 * 注销连接
 */
void resource_unregister_connection(void *conn_ptr);

/**
 * 注册线程
 */
int resource_register_thread(pthread_t thread_id);

/**
 * 注销线程
 */
void resource_unregister_thread(pthread_t thread_id);

/**
 * 检查资源使用情况
 */
double resource_get_usage_percent(resource_type_t type);

/**
 * 获取资源统计信息
 */
void resource_get_stats(resource_stats_t *stats);

/**
 * 强制执行资源清理
 */
int resource_force_cleanup(void);

/**
 * 设置资源告警回调
 */
void resource_set_alert_callback(resource_alert_callback_t callback);

/**
 * 打印资源使用报告
 */
void resource_print_report(void);

/**
 * 检查资源是否接近限制
 */
int resource_check_limits(void);

/**
 * 获取默认资源配置
 */
resource_config_t resource_get_default_config(void);

// 便利宏定义
#define RESOURCE_SAFE_MALLOC(size) \
    ({ \
        void *ptr = resource_malloc(size); \
        if (!ptr) { \
            log_error("内存分配失败: %zu 字节", size); \
        } \
        ptr; \
    })

#define RESOURCE_SAFE_FREE(ptr) \
    do { \
        if (ptr) { \
            resource_free(ptr); \
            ptr = NULL; \
        } \
    } while(0)

#define RESOURCE_CHECK_USAGE(type, threshold) \
    ({ \
        double usage = resource_get_usage_percent(type); \
        if (usage > threshold) { \
            log_warn("资源使用率过高: %s %.1f%%", \
                     #type, usage); \
        } \
        usage; \
    })

#endif // RESOURCE_MANAGER_H
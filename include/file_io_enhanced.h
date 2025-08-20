/**
 * 增强版文件I/O模块
 * 提供零拷贝、异步I/O、文件缓存等高性能文件处理功能
 */

#ifndef FILE_IO_ENHANCED_H
#define FILE_IO_ENHANCED_H

#include <sys/types.h>
#include <sys/stat.h>
#include <pthread.h>
#include <stdatomic.h>

// 文件缓存项
typedef struct file_cache_item {
    char *path;                    // 文件路径
    void *data;                    // 文件数据
    size_t size;                   // 文件大小
    time_t mtime;                  // 修改时间
    time_t access_time;            // 访问时间
    atomic_int ref_count;          // 引用计数
    atomic_int is_valid;           // 是否有效
    struct file_cache_item *next;  // 链表指针
} file_cache_item_t;

// 文件缓存管理器
typedef struct file_cache_manager {
    file_cache_item_t **buckets;   // 哈希桶
    size_t bucket_count;           // 桶数量
    size_t max_size;               // 最大缓存大小
    size_t current_size;           // 当前缓存大小
    pthread_mutex_t mutex;         // 互斥锁
    pthread_t cleanup_thread;      // 清理线程
    atomic_int stop_cleanup;       // 停止清理标志
} file_cache_manager_t;

// 异步文件读取上下文
typedef struct async_file_context {
    int fd;                        // 文件描述符
    void *buffer;                  // 缓冲区
    size_t buffer_size;            // 缓冲区大小
    size_t offset;                 // 读取偏移
    size_t total_size;             // 总大小
    int (*callback)(void *data, size_t size, void *arg); // 回调函数
    void *arg;                     // 回调参数
    pthread_t thread;              // 读取线程
    atomic_int completed;          // 完成标志
    atomic_int error;              // 错误标志
} async_file_context_t;

// 零拷贝文件传输上下文
typedef struct zero_copy_context {
    int client_fd;                 // 客户端文件描述符
    int file_fd;                   // 文件描述符
    void *mmap_addr;               // mmap地址
    size_t file_size;              // 文件大小
    size_t offset;                 // 当前偏移
    size_t sent_bytes;             // 已发送字节数
    atomic_int completed;          // 完成标志
    atomic_int error;              // 错误标志
} zero_copy_context_t;

// 文件I/O统计信息
typedef struct file_io_stats {
    atomic_uint_fast64_t total_requests;      // 总请求数
    atomic_uint_fast64_t cache_hits;          // 缓存命中数
    atomic_uint_fast64_t cache_misses;        // 缓存未命中数
    atomic_uint_fast64_t sendfile_requests;   // sendfile请求数
    atomic_uint_fast64_t mmap_requests;       // mmap请求数
    atomic_uint_fast64_t async_requests;      // 异步请求数
    atomic_uint_fast64_t total_bytes_sent;    // 总发送字节数
    atomic_uint_fast64_t total_read_time;     // 总读取时间(纳秒)
    atomic_uint_fast64_t total_send_time;     // 总发送时间(纳秒)
} file_io_stats_t;

// 文件I/O配置
typedef struct file_io_config {
    size_t cache_size;             // 缓存大小(MB)
    size_t max_file_size;          // 最大文件大小(MB)
    int enable_mmap;               // 是否启用mmap
    int enable_async;              // 是否启用异步I/O
    int enable_sendfile;           // 是否启用sendfile
    int cache_cleanup_interval;    // 缓存清理间隔(秒)
    size_t read_buffer_size;       // 读取缓冲区大小
    size_t write_buffer_size;      // 写入缓冲区大小
} file_io_config_t;

// 初始化文件I/O模块
int file_io_enhanced_init(const file_io_config_t *config);

// 销毁文件I/O模块
void file_io_enhanced_destroy(void);

// 获取文件I/O统计信息
void file_io_enhanced_get_stats(file_io_stats_t *stats);

// 重置文件I/O统计信息
void file_io_enhanced_reset_stats(void);

// 打印文件I/O统计信息
void file_io_enhanced_print_stats(void);

// 零拷贝文件发送（自动选择最优方式）
int file_io_enhanced_send_file(int client_fd, const char *file_path, size_t *sent_bytes);

// 使用mmap发送文件
int file_io_enhanced_send_file_mmap(int client_fd, const char *file_path, size_t *sent_bytes);

// 使用sendfile发送文件
int file_io_enhanced_send_file_sendfile(int client_fd, const char *file_path, size_t *sent_bytes);

// 异步读取文件
int file_io_enhanced_read_file_async(const char *file_path, 
                                    int (*callback)(void *data, size_t size, void *arg),
                                    void *arg);

// 从缓存获取文件
void *file_io_enhanced_get_from_cache(const char *file_path, size_t *size);

// 将文件添加到缓存
int file_io_enhanced_add_to_cache(const char *file_path, const void *data, size_t size);

// 从缓存移除文件
void file_io_enhanced_remove_from_cache(const char *file_path);

// 清空缓存
void file_io_enhanced_clear_cache(void);

// 预加载文件到缓存
int file_io_enhanced_preload_file(const char *file_path);

// 批量预加载文件
int file_io_enhanced_preload_files(const char **file_paths, int count);

// 获取文件信息（缓存友好）
int file_io_enhanced_get_file_info(const char *file_path, struct stat *st);

// 检查文件是否在缓存中
int file_io_enhanced_is_cached(const char *file_path);

// 获取缓存使用情况
void file_io_enhanced_get_cache_info(size_t *current_size, size_t *max_size, 
                                    size_t *hit_count, size_t *miss_count);

#endif /* FILE_IO_ENHANCED_H */ 
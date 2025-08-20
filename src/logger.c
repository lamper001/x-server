/**
 * X-Server 高性能日志系统 - TLS优化版本
 * 采用线程本地缓冲区 + 批量写入的混合架构
 * 最小化锁竞争，保证数据完整性
 */

#include "../include/logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/time.h>
#include <errno.h>
#include <sys/stat.h>
#include <pthread.h>

// TLS缓冲区配置
#define TLS_BUFFER_SIZE 8192        // 8KB线程本地缓冲区
#define BATCH_FLUSH_THRESHOLD 6144  // 6KB时触发批量刷新
#define MAX_LOG_ENTRY_SIZE 1024     // 单条日志最大长度
#define IDLE_FLUSH_INTERVAL 5       // 空闲5秒后强制刷新
#define PERIODIC_FLUSH_INTERVAL 30  // 定期30秒强制刷新

// 线程本地缓冲区结构
typedef struct {
    char buffer[TLS_BUFFER_SIZE];
    size_t write_pos;
    size_t flush_count;
    pthread_t thread_id;
    int initialized;
    time_t last_write_time;    // 最后写入时间
    time_t last_flush_time;    // 最后刷新时间
} tls_log_buffer_t;

// 全局共享缓冲区（用于批量写入）
typedef struct {
    char server_buffer[LOGGER_BUFFER_SIZE];
    char access_buffer[LOGGER_BUFFER_SIZE];
    size_t server_pos;
    size_t access_pos;
    time_t server_last_write_time;    // 服务器日志最后写入时间
    time_t access_last_write_time;    // 访问日志最后写入时间
    time_t server_last_flush_time;    // 服务器日志最后刷新时间
    time_t access_last_flush_time;    // 访问日志最后刷新时间
    pthread_mutex_t server_mutex;
    pthread_mutex_t access_mutex;
} global_log_buffer_t;

// 扩展性能统计结构
typedef struct {
    volatile uint64_t total_logs;
    volatile uint64_t total_bytes;
    volatile uint64_t flush_count;
    volatile uint64_t tls_flush_count;  // TLS缓冲区刷新次数
    volatile uint64_t drop_count;
    volatile uint64_t error_count;
} extended_logger_stats_t;

// 全局状态
static volatile int g_initialized = 0;
static logger_config_t g_config;
static FILE *g_server_log = NULL;
static FILE *g_access_log = NULL;
static pthread_mutex_t g_init_mutex = PTHREAD_MUTEX_INITIALIZER;

// 全局共享缓冲区
static global_log_buffer_t g_global_buffer = {
    .server_pos = 0,
    .access_pos = 0,
    .server_mutex = PTHREAD_MUTEX_INITIALIZER,
    .access_mutex = PTHREAD_MUTEX_INITIALIZER
};

// 性能统计
static extended_logger_stats_t g_stats = {0};

// 线程本地存储
static __thread tls_log_buffer_t *g_tls_buffer = NULL;
static __thread time_t g_cached_time = 0;
static __thread char g_cached_time_str[32] = {0};

// 原子操作辅助函数
static inline void atomic_add_uint64(volatile uint64_t *ptr, uint64_t value) {
    __sync_fetch_and_add(ptr, value);
}

// 获取格式化时间字符串（线程本地缓存）
static const char *get_time_string(void) {
    time_t now = time(NULL);
    if (now != g_cached_time) {
        struct tm *tm_info = localtime(&now);
        strftime(g_cached_time_str, sizeof(g_cached_time_str), 
                "%Y-%m-%d %H:%M:%S", tm_info);
        g_cached_time = now;
    }
    return g_cached_time_str;
}

// 获取高精度时间字符串（包含微秒）
static void get_precise_time_string(char *buffer, size_t size) {
    struct timeval tv;
    struct tm *tm_info;
    
    gettimeofday(&tv, NULL);
    tm_info = localtime(&tv.tv_sec);
    
    snprintf(buffer, size, "%04d-%02d-%02d %02d:%02d:%02d.%06d",
             tm_info->tm_year + 1900,
             tm_info->tm_mon + 1,
             tm_info->tm_mday,
             tm_info->tm_hour,
             tm_info->tm_min,
             tm_info->tm_sec,
             (int)tv.tv_usec);
}

// 初始化线程本地缓冲区
static int init_tls_buffer(void) {
    if (g_tls_buffer && g_tls_buffer->initialized) {
        return 0;
    }
    
    if (!g_tls_buffer) {
        g_tls_buffer = malloc(sizeof(tls_log_buffer_t));
        if (!g_tls_buffer) {
            return -1;
        }
    }
    
    time_t now = time(NULL);
    memset(g_tls_buffer, 0, sizeof(tls_log_buffer_t));
    g_tls_buffer->write_pos = 0;
    g_tls_buffer->flush_count = 0;
    g_tls_buffer->thread_id = pthread_self();
    g_tls_buffer->initialized = 1;
    g_tls_buffer->last_write_time = now;
    g_tls_buffer->last_flush_time = now;
    
    return 0;
}

// 检查是否需要刷新缓冲区
static int should_flush_buffer(int force_flush) {
    if (!g_tls_buffer || g_tls_buffer->write_pos == 0) {
        return 0;
    }
    
    // 强制刷新
    if (force_flush) {
        return 1;
    }
    
    // 缓冲区达到阈值
    if (g_tls_buffer->write_pos >= BATCH_FLUSH_THRESHOLD) {
        return 1;
    }
    
    time_t now = time(NULL);
    
    // 空闲时间超过阈值（5秒无新日志写入）
    if (now - g_tls_buffer->last_write_time >= IDLE_FLUSH_INTERVAL) {
        return 1;
    }
    
    // 定期刷新（30秒强制刷新一次）
    if (now - g_tls_buffer->last_flush_time >= PERIODIC_FLUSH_INTERVAL) {
        return 1;
    }
    
    return 0;
}

// 批量刷新TLS缓冲区到全局缓冲区
static void flush_tls_to_global(int is_server_log, int force_flush) {
    if (!should_flush_buffer(force_flush)) {
        return;
    }
    
    pthread_mutex_t *mutex;
    char *global_buf;
    size_t *global_pos;
    FILE *log_file;
    
    if (is_server_log) {
        mutex = &g_global_buffer.server_mutex;
        global_buf = g_global_buffer.server_buffer;
        global_pos = &g_global_buffer.server_pos;
        log_file = g_server_log;
    } else {
        mutex = &g_global_buffer.access_mutex;
        global_buf = g_global_buffer.access_buffer;
        global_pos = &g_global_buffer.access_pos;
        log_file = g_access_log;
    }
    
    // 加锁进行批量写入
    pthread_mutex_lock(mutex);
    
    // 检查全局缓冲区空间
    if (*global_pos + g_tls_buffer->write_pos >= LOGGER_BUFFER_SIZE) {
        // 全局缓冲区满，直接写入文件
        if (log_file && *global_pos > 0) {
            size_t written = fwrite(global_buf, 1, *global_pos, log_file);
            if (written == *global_pos) {
                fflush(log_file);
                *global_pos = 0;
                atomic_add_uint64(&g_stats.flush_count, 1);
            } else {
                atomic_add_uint64(&g_stats.error_count, 1);
            }
        }
    }
    
    // 将TLS缓冲区内容复制到全局缓冲区
    if (*global_pos + g_tls_buffer->write_pos < LOGGER_BUFFER_SIZE) {
        memcpy(global_buf + *global_pos, g_tls_buffer->buffer, g_tls_buffer->write_pos);
        *global_pos += g_tls_buffer->write_pos;
        atomic_add_uint64(&g_stats.total_bytes, g_tls_buffer->write_pos);
        
        // 更新全局缓冲区的最后写入时间
        time_t now = time(NULL);
        if (is_server_log) {
            g_global_buffer.server_last_write_time = now;
        } else {
            g_global_buffer.access_last_write_time = now;
        }
    } else {
        // 直接写入文件
        if (log_file) {
            size_t written = fwrite(g_tls_buffer->buffer, 1, g_tls_buffer->write_pos, log_file);
            if (written == g_tls_buffer->write_pos) {
                fflush(log_file);
                atomic_add_uint64(&g_stats.total_bytes, written);
                atomic_add_uint64(&g_stats.flush_count, 1);
                
                // 更新全局缓冲区的最后刷新时间
                time_t now = time(NULL);
                if (is_server_log) {
                    g_global_buffer.server_last_flush_time = now;
                } else {
                    g_global_buffer.access_last_flush_time = now;
                }
            } else {
                atomic_add_uint64(&g_stats.error_count, 1);
            }
        } else {
            atomic_add_uint64(&g_stats.drop_count, 1);
        }
    }
    
    pthread_mutex_unlock(mutex);
    
    // 重置TLS缓冲区并更新时间戳
    g_tls_buffer->write_pos = 0;
    g_tls_buffer->flush_count++;
    g_tls_buffer->last_flush_time = time(NULL);
    atomic_add_uint64(&g_stats.tls_flush_count, 1);
}

// 写入TLS缓冲区
static void write_to_tls_buffer(const char *data, size_t len, int is_server_log) {
    if (!g_initialized) {
        return;
    }
    
    // 初始化TLS缓冲区
    if (init_tls_buffer() != 0) {
        atomic_add_uint64(&g_stats.drop_count, 1);
        return;
    }
    
    // 检查单条日志大小
    if (len > MAX_LOG_ENTRY_SIZE) {
        atomic_add_uint64(&g_stats.drop_count, 1);
        return;
    }
    
    // 检查TLS缓冲区空间
    if (g_tls_buffer->write_pos + len >= TLS_BUFFER_SIZE) {
        // TLS缓冲区满，强制刷新到对应的全局缓冲区
        flush_tls_to_global(is_server_log, 1);
    }
    
    // 写入TLS缓冲区
    if (g_tls_buffer->write_pos + len < TLS_BUFFER_SIZE) {
        memcpy(g_tls_buffer->buffer + g_tls_buffer->write_pos, data, len);
        g_tls_buffer->write_pos += len;
        g_tls_buffer->last_write_time = time(NULL);  // 更新最后写入时间
        atomic_add_uint64(&g_stats.total_logs, 1);
        
        // 检查是否需要刷新（包括空闲和定期检查）
        flush_tls_to_global(is_server_log, 0);
    } else {
        atomic_add_uint64(&g_stats.drop_count, 1);
    }
}

// 获取日志文件名
static void get_log_filename(char *filename, size_t size, const char *prefix) {
    if (g_config.daily_rotation) {
        time_t now = time(NULL);
        struct tm *tm_info = localtime(&now);
        snprintf(filename, size, "%s/%s.%04d-%02d-%02d.log",
                g_config.log_dir, prefix,
                tm_info->tm_year + 1900,
                tm_info->tm_mon + 1,
                tm_info->tm_mday);
    } else {
        snprintf(filename, size, "%s/%s.log", g_config.log_dir, prefix);
    }
}

// 创建日志目录
static int create_log_directory(const char *path) {
    struct stat st = {0};
    if (stat(path, &st) == -1) {
        if (mkdir(path, 0755) == -1) {
            return -1;
        }
    }
    return 0;
}

// 初始化日志系统
int init_logger(const char *log_path, int level, int daily_rotation) {
    if (g_initialized) {
        return 0;
    }
    
    pthread_mutex_lock(&g_init_mutex);
    
    // 双重检查锁定
    if (g_initialized) {
        pthread_mutex_unlock(&g_init_mutex);
        return 0;
    }
    
    // 设置配置
    strncpy(g_config.log_dir, log_path ? log_path : "./logs", 
            sizeof(g_config.log_dir) - 1);
    g_config.level = level;
    g_config.daily_rotation = daily_rotation;
    g_config.buffer_size = LOGGER_BUFFER_SIZE;
    
    // 创建日志目录
    if (create_log_directory(g_config.log_dir) != 0) {
        pthread_mutex_unlock(&g_init_mutex);
        return -1;
    }
    
    // 初始化全局缓冲区
    memset(&g_global_buffer, 0, sizeof(global_log_buffer_t));
    time_t now = time(NULL);
    g_global_buffer.server_pos = 0;
    g_global_buffer.access_pos = 0;
    g_global_buffer.server_last_write_time = now;
    g_global_buffer.access_last_write_time = now;
    g_global_buffer.server_last_flush_time = now;
    g_global_buffer.access_last_flush_time = now;
    
    // 打开日志文件
    char server_filename[512], access_filename[512];
    get_log_filename(server_filename, sizeof(server_filename), "server");
    get_log_filename(access_filename, sizeof(access_filename), "access");
    
    g_server_log = fopen(server_filename, "a");
    g_access_log = fopen(access_filename, "a");
    
    if (!g_server_log || !g_access_log) {
        if (g_server_log) fclose(g_server_log);
        if (g_access_log) fclose(g_access_log);
        pthread_mutex_unlock(&g_init_mutex);
        return -1;
    }
    
    // 设置文件权限
    chmod(server_filename, 0640);
    chmod(access_filename, 0640);
    
    // 重置统计信息
    memset((void*)&g_stats, 0, sizeof(g_stats));
    
    g_initialized = 1;
    
    pthread_mutex_unlock(&g_init_mutex);
    
    // 记录初始化成功
    if (g_config.level <= LOG_LEVEL_INFO) {
        log_info("TLS优化日志系统初始化成功，目录: %s，级别: %d，TLS缓冲区: %dKB", 
                g_config.log_dir, g_config.level, TLS_BUFFER_SIZE/1024);
    }
    
    return 0;
}

// 更新日志配置
int update_logger_config(const char *log_path, int level, int daily_rotation) {
    if (!g_initialized) {
        // 在多进程环境中，如果日志系统未初始化，可能是因为这是Worker process
        // 而日志系统已经在Master进程中初始化过了
        // 此时应该避免重复初始化，只是静默返回成功
        return 0;  // 静默返回，避免重复初始化
    }
    
    pthread_mutex_lock(&g_init_mutex);
    
    // 强制刷新所有缓冲区
    logger_flush();
    
    // 更新配置
    if (log_path) {
        strncpy(g_config.log_dir, log_path, sizeof(g_config.log_dir) - 1);
    }
    g_config.level = level;
    g_config.daily_rotation = daily_rotation;
    
    pthread_mutex_unlock(&g_init_mutex);
    
    // 只在Master进程中输出配置更新日志
    if (getenv("WORKER_PROCESS_ID") == NULL && g_config.level <= LOG_LEVEL_INFO) {
        log_info("日志配置已更新，目录: %s，级别: %d", g_config.log_dir, g_config.level);
    }
    
    return 0;
}

// 关闭日志系统
void close_logger(void) {
    if (!g_initialized) {
        return;
    }
    
    pthread_mutex_lock(&g_init_mutex);
    
    if (!g_initialized) {
        pthread_mutex_unlock(&g_init_mutex);
        return;
    }
    
    // 强制刷新所有缓冲区
    logger_flush();
    
    // 刷新全局缓冲区到文件
    pthread_mutex_lock(&g_global_buffer.server_mutex);
    if (g_global_buffer.server_pos > 0 && g_server_log) {
        size_t written = fwrite(g_global_buffer.server_buffer, 1, g_global_buffer.server_pos, g_server_log);
        if (written == g_global_buffer.server_pos) {
            fflush(g_server_log);
        } else {
            atomic_add_uint64(&g_stats.error_count, 1);
        }
        g_global_buffer.server_pos = 0;
    }
    pthread_mutex_unlock(&g_global_buffer.server_mutex);
    
    pthread_mutex_lock(&g_global_buffer.access_mutex);
    if (g_global_buffer.access_pos > 0 && g_access_log) {
        size_t written = fwrite(g_global_buffer.access_buffer, 1, g_global_buffer.access_pos, g_access_log);
        if (written == g_global_buffer.access_pos) {
            fflush(g_access_log);
        } else {
            atomic_add_uint64(&g_stats.error_count, 1);
        }
        g_global_buffer.access_pos = 0;
    }
    pthread_mutex_unlock(&g_global_buffer.access_mutex);
    
    // 关闭文件
    if (g_server_log) {
        fclose(g_server_log);
        g_server_log = NULL;
    }
    if (g_access_log) {
        fclose(g_access_log);
        g_access_log = NULL;
    }
    
    g_initialized = 0;
    
    pthread_mutex_unlock(&g_init_mutex);
}

// 通用日志记录函数
static void log_message(log_level_t level, const char *format, va_list args) {
    if (!g_initialized || level < g_config.level) {
        return;
    }
    
    const char *level_str[] = {"DEBUG", "INFO", "WARN", "ERROR"};
    char log_line[MAX_LOG_ENTRY_SIZE];
    int len;
    
    // 格式化日志
    len = snprintf(log_line, sizeof(log_line), "[%s] [%s] ", 
                   get_time_string(), level_str[level]);
    if (len > 0 && len < MAX_LOG_ENTRY_SIZE - 1) {
        len += vsnprintf(log_line + len, sizeof(log_line) - len - 1, format, args);
        if (len > 0 && len < MAX_LOG_ENTRY_SIZE - 1) {
            log_line[len++] = '\n';
            log_line[len] = '\0';
            
            // 写入TLS缓冲区
            write_to_tls_buffer(log_line, len, 1);  // 1表示服务器日志
        }
    }
}

// API兼容函数
void log_debug(const char *format, ...) {
    va_list args;
    va_start(args, format);
    log_message(LOG_LEVEL_DEBUG, format, args);
    va_end(args);
}

void log_info(const char *format, ...) {
    va_list args;
    va_start(args, format);
    log_message(LOG_LEVEL_INFO, format, args);
    va_end(args);
}

void log_warn(const char *format, ...) {
    va_list args;
    va_start(args, format);
    log_message(LOG_LEVEL_WARN, format, args);
    va_end(args);
}

void log_error(const char *format, ...) {
    va_list args;
    va_start(args, format);
    log_message(LOG_LEVEL_ERROR, format, args);
    va_end(args);
}

// 访问日志 - 直接写入全局缓冲区
void log_access(const char *client_ip, const char *method, const char *path,
               int status_code, size_t response_size, const char *user_agent) {
    if (!g_initialized) {
        return;
    }
    
    char precise_time[64];
    get_precise_time_string(precise_time, sizeof(precise_time));
    
    char log_line[MAX_LOG_ENTRY_SIZE];
    int len = snprintf(log_line, sizeof(log_line),
                      "%s - - [%s] \"%s %s HTTP/1.1\" %d %zu \"-\" \"%s\"\n",
                      client_ip ? client_ip : "-",
                      precise_time,
                      method ? method : "-",
                      path ? path : "-",
                      status_code,
                      response_size,
                      user_agent ? user_agent : "-");
    
    if (len > 0 && len < MAX_LOG_ENTRY_SIZE) {
        // 直接写入访问日志全局缓冲区，避免TLS缓冲区混乱
        pthread_mutex_lock(&g_global_buffer.access_mutex);
        
        // 检查全局缓冲区空间
        if (g_global_buffer.access_pos + len >= LOGGER_BUFFER_SIZE) {
            // 全局缓冲区满，直接写入文件
            if (g_access_log && g_global_buffer.access_pos > 0) {
                size_t written = fwrite(g_global_buffer.access_buffer, 1, g_global_buffer.access_pos, g_access_log);
                if (written == g_global_buffer.access_pos) {
                    fflush(g_access_log);
                    atomic_add_uint64(&g_stats.flush_count, 1);
                    g_global_buffer.access_last_flush_time = time(NULL);
                } else {
                    atomic_add_uint64(&g_stats.error_count, 1);
                }
                g_global_buffer.access_pos = 0;
            }
        }
        
        // 写入全局缓冲区
        if (g_global_buffer.access_pos + len < LOGGER_BUFFER_SIZE) {
            memcpy(g_global_buffer.access_buffer + g_global_buffer.access_pos, log_line, len);
            g_global_buffer.access_pos += len;
            g_global_buffer.access_last_write_time = time(NULL);
            atomic_add_uint64(&g_stats.total_logs, 1);
            atomic_add_uint64(&g_stats.total_bytes, len);
        } else {
            // 直接写入文件
            if (g_access_log) {
                size_t written = fwrite(log_line, 1, (size_t)len, g_access_log);
                if (written == (size_t)len) {
                    fflush(g_access_log);
                    atomic_add_uint64(&g_stats.total_logs, 1);
                    atomic_add_uint64(&g_stats.total_bytes, len);
                    atomic_add_uint64(&g_stats.flush_count, 1);
                    g_global_buffer.access_last_flush_time = time(NULL);
                } else {
                    atomic_add_uint64(&g_stats.error_count, 1);
                }
            } else {
                atomic_add_uint64(&g_stats.drop_count, 1);
            }
        }
        
        pthread_mutex_unlock(&g_global_buffer.access_mutex);
    }
}

// 强制刷新缓冲区
void logger_flush(void) {
    if (!g_initialized) {
        return;
    }
    
    // 刷新当前线程的TLS缓冲区
    if (g_tls_buffer && g_tls_buffer->write_pos > 0) {
        flush_tls_to_global(1, 1);  // 强制刷新服务器日志
        flush_tls_to_global(0, 1);  // 强制刷新访问日志
    }
}

// 强制刷新全局缓冲区到文件
static void flush_global_buffers_to_file(void) {
    time_t now = time(NULL);
    
    // 刷新服务器日志全局缓冲区
    pthread_mutex_lock(&g_global_buffer.server_mutex);
    if (g_global_buffer.server_pos > 0 && g_server_log) {
        // 检查是否需要刷新（空闲5秒或定期30秒）
        if ((now - g_global_buffer.server_last_write_time >= IDLE_FLUSH_INTERVAL) ||
            (now - g_global_buffer.server_last_flush_time >= PERIODIC_FLUSH_INTERVAL)) {
            
            size_t written = fwrite(g_global_buffer.server_buffer, 1, g_global_buffer.server_pos, g_server_log);
            if (written == g_global_buffer.server_pos) {
                fflush(g_server_log);
                atomic_add_uint64(&g_stats.flush_count, 1);
                g_global_buffer.server_last_flush_time = now;
            } else {
                atomic_add_uint64(&g_stats.error_count, 1);
            }
            g_global_buffer.server_pos = 0;
        }
    }
    pthread_mutex_unlock(&g_global_buffer.server_mutex);
    
    // 刷新访问日志全局缓冲区
    pthread_mutex_lock(&g_global_buffer.access_mutex);
    if (g_global_buffer.access_pos > 0 && g_access_log) {
        // 检查是否需要刷新（空闲5秒或定期30秒）
        if ((now - g_global_buffer.access_last_write_time >= IDLE_FLUSH_INTERVAL) ||
            (now - g_global_buffer.access_last_flush_time >= PERIODIC_FLUSH_INTERVAL)) {
            
            size_t written = fwrite(g_global_buffer.access_buffer, 1, g_global_buffer.access_pos, g_access_log);
            if (written == g_global_buffer.access_pos) {
                fflush(g_access_log);
                atomic_add_uint64(&g_stats.flush_count, 1);
                g_global_buffer.access_last_flush_time = now;
            } else {
                atomic_add_uint64(&g_stats.error_count, 1);
            }
            g_global_buffer.access_pos = 0;
        }
    }
    pthread_mutex_unlock(&g_global_buffer.access_mutex);
}

// 检查并刷新空闲缓冲区（供主循环调用）
void logger_check_idle_flush(void) {
    if (!g_initialized) {
        return;
    }
    
    // 1. 检查当前线程的TLS缓冲区是否需要空闲刷新
    if (g_tls_buffer && g_tls_buffer->write_pos > 0) {
        time_t now = time(NULL);
        
        // 检查空闲时间或定期刷新
        if ((now - g_tls_buffer->last_write_time >= IDLE_FLUSH_INTERVAL) ||
            (now - g_tls_buffer->last_flush_time >= PERIODIC_FLUSH_INTERVAL)) {
            // 注意：TLS缓冲区可能包含服务器日志和访问日志的混合数据
            // 但由于我们无法区分，所以需要分别尝试刷新
            flush_tls_to_global(1, 1);  // 刷新到服务器日志全局缓冲区
            flush_tls_to_global(0, 1);  // 刷新到访问日志全局缓冲区
        }
    }
    
    // 2. 强制刷新全局缓冲区到文件（这是关键！）
    // 即使TLS缓冲区为空，全局缓冲区可能有其他线程写入的数据
    flush_global_buffers_to_file();
}

// 获取性能统计
void logger_get_stats(logger_stats_t *stats) {
    if (stats && g_initialized) {
        memcpy(stats, (void*)&g_stats, sizeof(logger_stats_t));
    }
}

// 重置统计信息
void logger_reset_stats(void) {
    if (g_initialized) {
        memset((void*)&g_stats, 0, sizeof(logger_stats_t));
    }
}

// 线程退出时清理TLS缓冲区
void logger_thread_cleanup(void) {
    if (g_tls_buffer) {
        // 刷新剩余数据
        if (g_tls_buffer->write_pos > 0) {
            flush_tls_to_global(1, 1);
            flush_tls_to_global(0, 1);
        }
        
        // 释放TLS缓冲区
        free(g_tls_buffer);
        g_tls_buffer = NULL;
    }
}
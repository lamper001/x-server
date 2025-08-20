/**
 * X-Server 高性能日志系统头文件
 * 统一的日志接口，集成高性能优化和原有API兼容性
 */

#ifndef LOGGER_H
#define LOGGER_H

#include <stdint.h>
#include <pthread.h>

// 日志级别定义
typedef enum {
    LOG_LEVEL_DEBUG = 0,
    LOG_LEVEL_INFO = 1,
    LOG_LEVEL_WARN = 2,
    LOG_LEVEL_ERROR = 3
} log_level_t;

// 配置常量
#define MAX_LOG_PATH_LEN 256
#define MAX_LOG_LINE_SIZE 2048
#define LOGGER_BUFFER_SIZE (64 * 1024)  // 64KB 缓冲区

// 日志配置结构
typedef struct {
    char log_dir[MAX_LOG_PATH_LEN];     // 日志目录
    log_level_t level;                  // 日志级别
    int daily_rotation;                 // 是否按天切分
    size_t buffer_size;                 // 缓冲区大小
} logger_config_t;

// 性能统计结构
typedef struct {
    uint64_t total_logs;        // 总日志数
    uint64_t total_bytes;       // 总字节数
    uint64_t flush_count;       // 刷新次数
    uint64_t drop_count;        // 丢弃日志数
    uint64_t error_count;       // 错误次数
} logger_stats_t;

// 核心API函数
int init_logger(const char *log_path, int level, int daily_rotation);
int update_logger_config(const char *log_path, int level, int daily_rotation);
void close_logger(void);

// 日志记录函数（原有API兼容）
void log_debug(const char *format, ...);
void log_info(const char *format, ...);
void log_warn(const char *format, ...);
void log_error(const char *format, ...);

// 访问日志函数
void log_access(const char *client_ip, const char *method, const char *path,
               int status_code, size_t response_size, const char *user_agent);

// 高性能扩展函数
void logger_flush(void);                        // 强制刷新缓冲区
void logger_check_idle_flush(void);             // 检查并刷新空闲缓冲区
void logger_get_stats(logger_stats_t *stats);   // 获取性能统计
void logger_reset_stats(void);                  // 重置统计信息

#endif // LOGGER_H
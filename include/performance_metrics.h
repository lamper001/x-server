/**
 * 性能指标监控模块 - 统一性能数据收集和分析
 */

#ifndef PERFORMANCE_METRICS_H
#define PERFORMANCE_METRICS_H

#include <stdint.h>
#include <time.h>
#include <sys/time.h>

// 性能指标类型
typedef enum {
    METRIC_REQUEST_COUNT,           // 请求总数
    METRIC_REQUEST_DURATION,        // 请求处理时间
    METRIC_CONNECTION_COUNT,        // 连接数
    METRIC_MEMORY_USAGE,           // 内存使用量
    METRIC_CPU_USAGE,              // CPU使用率
    METRIC_NETWORK_IO,             // 网络I/O
    METRIC_FILE_IO,                // 文件I/O
    METRIC_ERROR_COUNT,            // 错误计数
    METRIC_CACHE_HIT_RATE,         // 缓存命中率
    METRIC_PROXY_LATENCY           // 代理延迟
} metric_type_t;

// 性能统计结构
typedef struct {
    uint64_t total_requests;        // 总请求数
    uint64_t successful_requests;   // 成功请求数
    uint64_t failed_requests;       // 失败请求数
    uint64_t total_bytes_sent;      // 发送字节总数
    uint64_t total_bytes_received;  // 接收字节总数
    
    // 时间统计
    uint64_t min_response_time_ns;  // 最小响应时间(纳秒)
    uint64_t max_response_time_ns;  // 最大响应时间(纳秒)
    uint64_t avg_response_time_ns;  // 平均响应时间(纳秒)
    
    // 连接统计
    uint32_t active_connections;    // 当前活跃连接数
    uint32_t max_connections;       // 最大连接数
    uint64_t total_connections;     // 总连接数
    
    // 内存统计
    size_t memory_used;             // 已使用内存
    size_t memory_peak;             // 内存使用峰值
    
    // 错误统计
    uint32_t error_4xx_count;       // 4xx错误数
    uint32_t error_5xx_count;       // 5xx错误数
    uint32_t timeout_count;         // 超时错误数
    
    // 缓存统计
    uint64_t cache_hits;            // 缓存命中数
    uint64_t cache_misses;          // 缓存未命中数
    
    time_t start_time;              // 统计开始时间
    time_t last_update;             // 最后更新时间
} performance_stats_t;

// 性能监控配置
typedef struct {
    int enable_detailed_metrics;    // 启用详细指标
    int collection_interval;        // 收集间隔(秒)
    int history_size;              // 历史数据大小
    char output_file[256];         // 输出文件路径
} metrics_config_t;

// 函数声明

/**
 * 初始化性能监控模块
 */
int performance_metrics_init(const metrics_config_t *config);

/**
 * 销毁性能监控模块
 */
void performance_metrics_destroy(void);

/**
 * 记录请求开始
 */
uint64_t performance_record_request_start(void);

/**
 * 记录请求结束
 */
void performance_record_request_end(uint64_t start_time, int status_code, size_t response_size);

/**
 * 更新连接统计
 */
void performance_update_connection_stats(int active_count, int is_new_connection);

/**
 * 更新内存统计
 */
void performance_update_memory_stats(size_t current_usage);

/**
 * 记录错误
 */
void performance_record_error(int status_code);

/**
 * 更新缓存统计
 */
void performance_update_cache_stats(int is_hit);

/**
 * 获取当前性能统计
 */
void performance_get_stats(performance_stats_t *stats);

/**
 * 重置性能统计
 */
void performance_reset_stats(void);

/**
 * 打印性能报告
 */
void performance_print_report(void);

/**
 * 导出性能数据到文件
 */
int performance_export_to_file(const char *filename);

/**
 * 获取高精度时间戳(纳秒)
 */
static inline uint64_t get_timestamp_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/**
 * 计算时间差(纳秒)
 */
static inline uint64_t calculate_duration_ns(uint64_t start_time) {
    return get_timestamp_ns() - start_time;
}

#endif // PERFORMANCE_METRICS_H
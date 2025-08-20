/**
 * 性能监控模块头文件
 * 提供实时性能监控和统计功能
 */

#ifndef PERFORMANCE_MONITOR_H
#define PERFORMANCE_MONITOR_H

#include <time.h>
#include <stdatomic.h>

// 性能指标结构
typedef struct {
    atomic_uint_fast64_t total_requests;      // 总请求数
    atomic_uint_fast64_t successful_requests; // 成功请求数
    atomic_uint_fast64_t failed_requests;     // 失败请求数
    atomic_uint_fast64_t total_bytes_sent;    // 总发送字节数
    atomic_uint_fast64_t total_bytes_received; // 总接收字节数
    atomic_uint_fast32_t active_connections;  // 活跃连接数
    atomic_uint_fast32_t peak_connections;    // 峰值连接数
    atomic_uint_fast64_t total_response_time; // 总响应时间
    atomic_uint_fast64_t min_response_time;   // 最小响应时间
    atomic_uint_fast64_t max_response_time;   // 最大响应时间
    time_t start_time;                        // 启动时间
    time_t last_reset_time;                   // 最后重置时间
} performance_metrics_t;

// 性能监控配置
typedef struct {
    int enable_monitoring;           // 启用监控
    int metrics_interval;            // 指标收集间隔（秒）
    int alert_threshold;             // 告警阈值
    int enable_auto_scaling;         // 启用自动扩缩容
    int max_cpu_usage;               // 最大CPU使用率
    int max_memory_usage;            // 最大内存使用率
} performance_config_t;

// 函数声明

/**
 * 初始化性能监控
 * 
 * @param config 监控配置
 * @return 成功返回0，失败返回-1
 */
int init_performance_monitor(const performance_config_t *config);

/**
 * 更新请求统计
 * 
 * @param response_time 响应时间（微秒）
 * @param bytes_sent 发送字节数
 * @param bytes_received 接收字节数
 * @param success 是否成功
 */
void update_request_stats(uint64_t response_time, size_t bytes_sent, 
                         size_t bytes_received, int success);

/**
 * 更新连接统计
 * 
 * @param connection_count 当前连接数
 */
void update_connection_stats(uint32_t connection_count);

/**
 * 获取性能指标
 * 
 * @param metrics 指标结构体指针
 */
void get_performance_metrics(performance_metrics_t *metrics);

/**
 * 重置性能指标
 */
void reset_performance_metrics(void);

/**
 * 生成性能报告
 * 
 * @param report_file 报告文件路径
 * @return 成功返回0，失败返回-1
 */
int generate_performance_report(const char *report_file);

/**
 * 检查是否需要自动扩缩容
 * 
 * @return 需要扩容返回1，需要缩容返回-1，无需操作返回0
 */
int check_auto_scaling_needs(void);

/**
 * 清理性能监控
 */
void cleanup_performance_monitor(void);

#endif /* PERFORMANCE_MONITOR_H */ 
/**
 * 连接限制模块头文件
 * 提供nginx风格的连接数和请求频率限制功能
 */

#ifndef CONNECTION_LIMIT_H
#define CONNECTION_LIMIT_H

#include <time.h>

// 哈希表大小
#define IP_HASH_SIZE 1024

// IP连接跟踪结构
typedef struct ip_connection_s {
    char ip[46];                    // IPv4/IPv6地址
    int connection_count;           // 当前连接数
    time_t last_access;            // 最后访问时间
    struct ip_connection_s *next;   // 链表指针
} ip_connection_t;

// IP请求频率限制结构
typedef struct ip_rate_limit_s {
    char ip[46];                    // IPv4/IPv6地址
    int request_count;              // 当前窗口内请求数
    int burst_count;                // 突发请求计数
    time_t last_request;           // 最后请求时间
    time_t window_start;           // 当前窗口开始时间
    struct ip_rate_limit_s *next;   // 链表指针
} ip_rate_limit_t;

// 连接限制配置
typedef struct {
    int max_connections_per_ip;     // 每IP最大连接数
    int max_requests_per_second;    // 每秒最大请求数
    int max_requests_burst;         // 最大突发请求数
    int cleanup_interval;           // 清理间隔（秒）
    int enable_connection_limit;    // 启用连接限制
    int enable_rate_limit;          // 启用速率限制
    int enable_ddos_protection;     // 启用DDoS防护
    int ddos_threshold;             // DDoS检测阈值
    int ddos_window;                // DDoS检测时间窗口
    int enable_geo_blocking;        // 启用地理位置限制
    char *blocked_countries;        // 被阻止的国家列表
} connection_limit_config_t;

// IP连接统计信息
typedef struct {
    int connection_count;           // 当前连接数
    int request_count;              // 当前请求数
    int burst_count;                // 突发请求数
    time_t last_access;            // 最后访问时间
    time_t last_request;           // 最后请求时间
} ip_connection_stats_t;

// 全局限制统计信息
typedef struct {
    int total_tracked_ips;          // 跟踪的IP总数
    int total_connections;          // 总连接数
    int total_requests;             // 总请求数
    int total_burst_requests;       // 总突发请求数
} global_limit_stats_t;

// 函数声明

/**
 * 检查连接限制
 * @param client_ip 客户端IP地址
 * @return 0=允许连接, -1=拒绝连接
 */
int check_connection_limit(const char *client_ip);

/**
 * 释放连接
 * @param client_ip 客户端IP地址
 */
void release_connection(const char *client_ip);

/**
 * 检查请求频率限制
 * @param client_ip 客户端IP地址
 * @return 0=允许请求, -1=拒绝请求
 */
int check_rate_limit(const char *client_ip);

/**
 * 获取IP连接统计信息
 * @param client_ip 客户端IP地址
 * @param stats 统计信息结构体指针
 * @return 0=成功, -1=失败
 */
int get_ip_connection_stats(const char *client_ip, ip_connection_stats_t *stats);

/**
 * 配置连接限制参数
 * @param config 配置结构体指针
 */
void configure_connection_limit(const connection_limit_config_t *config);

/**
 * 从服务器配置更新连接限制
 * 
 * @param max_connections_per_ip 每个IP的最大连接数
 * @param cleanup_interval 清理间隔（秒）
 */
void update_connection_limit_from_config(int max_connections_per_ip, int cleanup_interval);

/**
 * 获取当前连接限制配置
 * @param config 配置结构体指针
 */
void get_connection_limit_config(connection_limit_config_t *config);

/**
 * 获取全局限制统计信息
 * @param stats 统计信息结构体指针
 */
void get_global_limit_stats(global_limit_stats_t *stats);

/**
 * 清理所有限制记录
 */
void cleanup_all_limits(void);

#endif // CONNECTION_LIMIT_H
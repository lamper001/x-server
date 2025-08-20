/**
 * 连接池优化模块头文件
 * 第四阶段：连接处理优化
 */

#ifndef CONNECTION_POOL_H
#define CONNECTION_POOL_H

#include "connection.h"
#include "config.h"
#include <pthread.h>
#include <time.h>

// 连接状态枚举
typedef enum {
    CONN_STATE_IDLE = 0,        // 空闲状态
    CONN_STATE_ACTIVE,          // 活跃状态
    CONN_STATE_READING,         // 读取状态
    CONN_STATE_WRITING,         // 写入状态
    CONN_STATE_CLOSING,         // 关闭中
    CONN_STATE_CLOSED           // 已关闭
} connection_state_t;

// 连接池统计信息
typedef struct {
    atomic_int total_connections;    // 总连接数
    atomic_int active_connections;   // 活跃连接数
    atomic_int idle_connections;     // 空闲连接数
    atomic_int reused_connections;   // 复用连接数
    atomic_int created_connections;  // 新建连接数
    atomic_int closed_connections;   // 关闭连接数
    atomic_int timeout_connections;  // 超时连接数
    uint64_t total_requests;         // 总请求数
    uint64_t total_bytes_read;       // 总读取字节数
    uint64_t total_bytes_written;    // 总写入字节数
    double avg_connection_lifetime;  // 平均连接生命周期
    double avg_requests_per_conn;    // 平均每连接请求数
} connection_pool_stats_t;

// 连接池配置
typedef struct {
    int max_connections;             // 最大连接数
    int min_idle_connections;        // 最小空闲连接数
    int max_idle_connections;        // 最大空闲连接数
    int connection_timeout;          // 连接超时时间（秒）
    int idle_timeout;                // 空闲超时时间（秒）
    int keepalive_timeout;           // Keep-Alive超时时间（秒）
    int max_requests_per_conn;       // 每连接最大请求数
    int enable_connection_reuse;     // 是否启用连接复用
    int enable_connection_pooling;   // 是否启用连接池
    int pool_cleanup_interval;       // 池清理间隔（秒）
} connection_pool_config_t;

// 连接池结构体
typedef struct connection_pool connection_pool_t;

/**
 * 创建连接池
 * 
 * @param config 连接池配置
 * @return 连接池指针，失败返回NULL
 */
connection_pool_t *connection_pool_create(const connection_pool_config_t *config);

/**
 * 销毁连接池
 * 
 * @param pool 连接池指针
 */
void connection_pool_destroy(connection_pool_t *pool);

/**
 * 从连接池获取连接
 * 
 * @param pool 连接池指针
 * @param fd 连接套接字
 * @param loop 事件循环
 * @param config 服务器配置
 * @param client_addr 客户端地址
 * @return 连接指针，失败返回NULL
 */
connection_t *connection_pool_get_connection(connection_pool_t *pool, int fd, 
                                           void *loop, int is_enhanced_loop,
                                           config_t *config, struct sockaddr_in *client_addr);

/**
 * 将连接归还到连接池
 * 
 * @param pool 连接池指针
 * @param conn 连接指针
 */
void connection_pool_return_connection(connection_pool_t *pool, connection_t *conn);

/**
 * 关闭连接（从池中移除）
 * 
 * @param pool 连接池指针
 * @param conn 连接指针
 */
void connection_pool_close_connection(connection_pool_t *pool, connection_t *conn);

/**
 * 获取连接池统计信息
 * 
 * @param pool 连接池指针
 * @param stats 统计信息结构体
 */
void connection_pool_get_stats(connection_pool_t *pool, connection_pool_stats_t *stats);

/**
 * 重置连接池统计信息
 * 
 * @param pool 连接池指针
 */
void connection_pool_reset_stats(connection_pool_t *pool);

/**
 * 打印连接池统计信息
 * 
 * @param pool 连接池指针
 */
void connection_pool_print_stats(connection_pool_t *pool);

/**
 * 清理空闲连接
 * 
 * @param pool 连接池指针
 * @return 清理的连接数
 */
int connection_pool_cleanup_idle(connection_pool_t *pool);

/**
 * 设置连接池配置
 * 
 * @param pool 连接池指针
 * @param config 新配置
 * @return 成功返回0，失败返回非0值
 */
int connection_pool_set_config(connection_pool_t *pool, const connection_pool_config_t *config);

/**
 * 获取连接池配置
 * 
 * @param pool 连接池指针
 * @param config 配置结构体
 */
void connection_pool_get_config(connection_pool_t *pool, connection_pool_config_t *config);

/**
 * 从配置文件中加载连接池配置
 * 
 * @param config 服务器配置
 * @return 连接池配置结构体
 */
connection_pool_config_t connection_pool_load_config(const config_t *config);

#endif /* CONNECTION_POOL_H */ 
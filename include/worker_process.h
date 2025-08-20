/**
 * Worker进程管理模块
 * 负责处理实际的HTTP请求
 */

#ifndef WORKER_PROCESS_H
#define WORKER_PROCESS_H

#include <sys/types.h>
#include <stdatomic.h>
#include <pthread.h>
#include "config.h"
#include "event_loop.h"
#include "event_loop.h"
#include "connection_pool.h"

// Worker进程状态
typedef enum {
    WORKER_STARTING,
    WORKER_RUNNING,
    WORKER_RELOADING,
    WORKER_STOPPING,
    WORKER_STOPPED
} worker_state_t;

// Worker进程上下文
typedef struct worker_context {
    int worker_id;
    pid_t worker_pid;
    worker_state_t state;
    
    // 事件循环（支持标准版和增强版）
    union {
        event_loop_t *event_loop;
    };
    int is_enhanced_loop;  // 是否使用增强版事件循环
    int listen_fd;
    
    // 配置
    config_t *config;
    
    // 统计信息 - 使用原子操作保证线程安全
    time_t start_time;
    atomic_uint_fast64_t requests_processed;
    atomic_uint_fast64_t total_requests;  // 总请求数
    atomic_uint_fast64_t bytes_sent;
    atomic_uint_fast64_t bytes_received;
    atomic_uint_fast32_t active_connections;
    atomic_uint_fast32_t total_connections;
    
    // 线程安全保护
    pthread_mutex_t stats_mutex;
} worker_context_t;

/**
 * Worker进程主函数
 * 
 * @param worker_id Worker进程ID
 * @param listen_fd 监听套接字
 * @param config 配置信息
 * @return 退出码
 */
int worker_process_run(int worker_id, int listen_fd, config_t *config);

/**
 * 获取Worker进程上下文
 * 
 * @return Worker上下文指针
 */
worker_context_t *get_worker_context(void);

/**
 * Worker进程优雅关闭
 */
void worker_graceful_shutdown(void);

/**
 * 更新Worker进程统计信息到共享内存（线程安全版本）
 * 
 * @param bytes_sent 发送字节数
 * @param bytes_received 接收字节数
 */
void update_worker_stats_safe(size_t bytes_sent, size_t bytes_received);

/**
 * 安全地增加连接计数
 */
void increment_connection_count_safe(void);

/**
 * 安全地减少连接计数
 */
void decrement_connection_count_safe(void);

/**
 * 获取Worker进程的连接池
 * 
 * @return 连接池指针
 */
connection_pool_t *get_worker_connection_pool(void);

#endif /* WORKER_PROCESS_H */
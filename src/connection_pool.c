/**
 * 连接池优化模块实现
 * 第四阶段：连接处理优化
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <pthread.h>
#include <assert.h>

#include "../include/connection_pool.h"
#include "../include/connection.h"
#include "../include/logger.h"
#include "../include/memory_pool.h"

// 前向声明，避免循环依赖
struct connection;

// 连接池内部结构
struct connection_pool {
    connection_pool_config_t config;           // 连接池配置
    connection_pool_stats_t stats;             // 统计信息
    
    // 连接管理
    connection_t **connections;                // 连接数组
    int connection_count;                      // 当前连接数
    int connection_capacity;                   // 连接数组容量
    
    // 空闲连接管理
    connection_t **idle_connections;           // 空闲连接数组
    int idle_count;                           // 空闲连接数
    int idle_capacity;                        // 空闲连接数组容量
    
    // 线程安全
    pthread_mutex_t pool_mutex;               // 连接池锁
    pthread_mutex_t idle_mutex;               // 空闲连接锁
    pthread_mutex_t stats_mutex;              // 统计信息锁
    
    // 清理线程
    pthread_t cleanup_thread;                 // 清理线程
    int cleanup_running;                      // 清理线程运行标志
    
    // 内存池
    void *memory_pool;                        // 内存池
};

// 连接池清理线程函数
static void *cleanup_thread_func(void *arg) {
    connection_pool_t *pool = (connection_pool_t *)arg;
    
    while (pool->cleanup_running) {
        sleep(pool->config.pool_cleanup_interval);
        
        if (pool->cleanup_running) {
            connection_pool_cleanup_idle(pool);
        }
    }
    
    return NULL;
}

// 创建连接池
connection_pool_t *connection_pool_create(const connection_pool_config_t *config) {
    connection_pool_t *pool = malloc(sizeof(connection_pool_t));
    if (!pool) {
        log_error("创建连接池失败：内存分配失败");
        return NULL;
    }
    
    // 初始化配置
    memcpy(&pool->config, config, sizeof(connection_pool_config_t));
    
    // 初始化统计信息
    memset(&pool->stats, 0, sizeof(connection_pool_stats_t));
    
    // 初始化连接数组
    pool->connection_capacity = config->max_connections;
    pool->connections = calloc(pool->connection_capacity, sizeof(connection_t*));
    if (!pool->connections) {
        log_error("创建连接池失败：连接数组分配失败");
        free(pool);
        return NULL;
    }
    pool->connection_count = 0;
    
    // 初始化空闲连接数组
    pool->idle_capacity = config->max_idle_connections;
    pool->idle_connections = calloc(pool->idle_capacity, sizeof(connection_t*));
    if (!pool->idle_connections) {
        log_error("创建连接池失败：空闲连接数组分配失败");
        free(pool->connections);
        free(pool);
        return NULL;
    }
    pool->idle_count = 0;
    
    // 初始化锁
    if (pthread_mutex_init(&pool->pool_mutex, NULL) != 0) {
        log_error("创建连接池失败：初始化连接池锁失败");
        free(pool->idle_connections);
        free(pool->connections);
        free(pool);
        return NULL;
    }
    
    if (pthread_mutex_init(&pool->idle_mutex, NULL) != 0) {
        log_error("创建连接池失败：初始化空闲连接锁失败");
        pthread_mutex_destroy(&pool->pool_mutex);
        free(pool->idle_connections);
        free(pool->connections);
        free(pool);
        return NULL;
    }
    
    if (pthread_mutex_init(&pool->stats_mutex, NULL) != 0) {
        log_error("创建连接池失败：初始化统计锁失败");
        pthread_mutex_destroy(&pool->idle_mutex);
        pthread_mutex_destroy(&pool->pool_mutex);
        free(pool->idle_connections);
        free(pool->connections);
        free(pool);
        return NULL;
    }
    
    // 初始化内存池 - 使用标准内存池函数
    pool->memory_pool = create_memory_pool(1024 * 1024); // 1MB
    if (!pool->memory_pool) {
        log_warn("创建连接池内存池失败，使用系统内存分配");
    }
    
    // 启动清理线程
    pool->cleanup_running = 1;
    if (pthread_create(&pool->cleanup_thread, NULL, cleanup_thread_func, pool) != 0) {
        log_error("创建连接池失败：启动清理线程失败");
        pthread_mutex_destroy(&pool->stats_mutex);
        pthread_mutex_destroy(&pool->idle_mutex);
        pthread_mutex_destroy(&pool->pool_mutex);
        if (pool->memory_pool) {
            destroy_memory_pool(pool->memory_pool);
        }
        free(pool->idle_connections);
        free(pool->connections);
        free(pool);
        return NULL;
    }
    
    log_info("连接池创建成功：最大连接数=%d, 最大空闲连接数=%d", 
             config->max_connections, config->max_idle_connections);
    
    return pool;
}

// Destroy connection pool
void connection_pool_destroy(connection_pool_t *pool) {
    if (!pool) {
        return;
    }
    
    // 停止清理线程
    pool->cleanup_running = 0;
    pthread_join(pool->cleanup_thread, NULL);
    
    // 关闭所有连接
    pthread_mutex_lock(&pool->pool_mutex);
    for (int i = 0; i < pool->connection_count; i++) {
        if (pool->connections[i]) {
            connection_destroy(pool->connections[i]);
            pool->connections[i] = NULL;
        }
    }
    pthread_mutex_unlock(&pool->pool_mutex);
    
    // 销毁锁
    pthread_mutex_destroy(&pool->stats_mutex);
    pthread_mutex_destroy(&pool->idle_mutex);
    pthread_mutex_destroy(&pool->pool_mutex);
    
    // 销毁内存池
    if (pool->memory_pool) {
        destroy_memory_pool(pool->memory_pool);
    }
    
    // 释放数组
    free(pool->idle_connections);
    free(pool->connections);
    
    log_info("连接池销毁完成");
    
    free(pool);
}

// 从连接池获取连接
connection_t *connection_pool_get_connection(connection_pool_t *pool, int fd, 
                                           void *loop, int is_enhanced_loop,
                                           config_t *config, struct sockaddr_in *client_addr) {
    if (!pool || !config) {
        return NULL;
    }
    
    connection_t *conn = NULL;
    
    // 尝试从空闲连接池获取
    if (pool->config.enable_connection_reuse) {
        pthread_mutex_lock(&pool->idle_mutex);
        
        if (pool->idle_count > 0) {
            // 获取最后一个空闲连接
            conn = pool->idle_connections[--pool->idle_count];
            pool->idle_connections[pool->idle_count] = NULL;
            
            // 更新统计信息
            pthread_mutex_lock(&pool->stats_mutex);
            atomic_fetch_add(&pool->stats.reused_connections, 1);
            atomic_fetch_add(&pool->stats.idle_connections, -1);
            atomic_fetch_add(&pool->stats.active_connections, 1);
            pthread_mutex_unlock(&pool->stats_mutex);
            
            log_debug("复用空闲连接：fd=%d", (int)(long)conn);
        }
        
        pthread_mutex_unlock(&pool->idle_mutex);
    }
    
    // 如果没有可复用的连接，创建新连接
    if (!conn) {
        pthread_mutex_lock(&pool->pool_mutex);
        
        // 检查连接数限制
        if (pool->connection_count >= pool->config.max_connections) {
            log_warn("连接池已满，无法创建新连接：当前=%d, 最大=%d", 
                     pool->connection_count, pool->config.max_connections);
            pthread_mutex_unlock(&pool->pool_mutex);
            return NULL;
        }
        
        // 创建新连接
        if (is_enhanced_loop) {
            conn = connection_create_enhanced(fd, (event_loop_t*)loop, config, client_addr);
        } else {
            conn = connection_create(fd, (event_loop_t*)loop, config, client_addr);
        }
        
        if (conn) {
            // 添加到连接数组
            pool->connections[pool->connection_count++] = conn;
            
            // 更新统计信息
            pthread_mutex_lock(&pool->stats_mutex);
            atomic_fetch_add(&pool->stats.created_connections, 1);
            atomic_fetch_add(&pool->stats.total_connections, 1);
            atomic_fetch_add(&pool->stats.active_connections, 1);
            pthread_mutex_unlock(&pool->stats_mutex);
            
            log_debug("创建新连接：fd=%d, 当前连接数=%d, 增强版=%d", fd, pool->connection_count, is_enhanced_loop);
        }
        
        pthread_mutex_unlock(&pool->pool_mutex);
    }
    
    return conn;
}

// 将连接归还到连接池
void connection_pool_return_connection(connection_pool_t *pool, connection_t *conn) {
    if (!pool || !conn) {
        return;
    }
    
    // 检查连接是否可以复用 - 暂时简化逻辑
    if (pool->config.enable_connection_reuse && 
        pool->idle_count < pool->config.max_idle_connections) {
        
        pthread_mutex_lock(&pool->idle_mutex);
        
        if (pool->idle_count < pool->idle_capacity) {
            pool->idle_connections[pool->idle_count++] = conn;
            
            // 更新统计信息
            pthread_mutex_lock(&pool->stats_mutex);
            atomic_fetch_add(&pool->stats.idle_connections, 1);
            atomic_fetch_add(&pool->stats.active_connections, -1);
            pthread_mutex_unlock(&pool->stats_mutex);
            
            log_debug("连接归还到空闲池：conn=%p, 空闲连接数=%d", conn, pool->idle_count);
        } else {
            // 空闲池已满，直接关闭连接
            connection_pool_close_connection(pool, conn);
        }
        
        pthread_mutex_unlock(&pool->idle_mutex);
    } else {
        // 不能复用，直接关闭
        connection_pool_close_connection(pool, conn);
    }
}

// 关闭连接（从池中移除）
void connection_pool_close_connection(connection_pool_t *pool, connection_t *conn) {
    if (!pool || !conn) {
        return;
    }
    
    pthread_mutex_lock(&pool->pool_mutex);
    
    // 从连接数组中移除
    for (int i = 0; i < pool->connection_count; i++) {
        if (pool->connections[i] == conn) {
            // 移动后面的连接
            for (int j = i; j < pool->connection_count - 1; j++) {
                pool->connections[j] = pool->connections[j + 1];
            }
            pool->connections[--pool->connection_count] = NULL;
            break;
        }
    }
    
    // 从空闲连接数组中移除
    pthread_mutex_lock(&pool->idle_mutex);
    for (int i = 0; i < pool->idle_count; i++) {
        if (pool->idle_connections[i] == conn) {
            // 移动后面的连接
            for (int j = i; j < pool->idle_count - 1; j++) {
                pool->idle_connections[j] = pool->idle_connections[j + 1];
            }
            pool->idle_connections[--pool->idle_count] = NULL;
            break;
        }
    }
    pthread_mutex_unlock(&pool->idle_mutex);
    
    // 更新统计信息
    pthread_mutex_lock(&pool->stats_mutex);
    atomic_fetch_add(&pool->stats.closed_connections, 1);
    atomic_fetch_add(&pool->stats.active_connections, -1);
    pthread_mutex_unlock(&pool->stats_mutex);
    
    pthread_mutex_unlock(&pool->pool_mutex);
    
    // 销毁连接
    connection_destroy(conn);
    
    log_debug("连接已关闭：conn=%p, 当前连接数=%d", conn, pool->connection_count);
}

// 获取连接池统计信息
void connection_pool_get_stats(connection_pool_t *pool, connection_pool_stats_t *stats) {
    if (!pool || !stats) {
        return;
    }
    
    pthread_mutex_lock(&pool->stats_mutex);
    memcpy(stats, &pool->stats, sizeof(connection_pool_stats_t));
    pthread_mutex_unlock(&pool->stats_mutex);
}

// 重置连接池统计信息
void connection_pool_reset_stats(connection_pool_t *pool) {
    if (!pool) {
        return;
    }
    
    pthread_mutex_lock(&pool->stats_mutex);
    memset(&pool->stats, 0, sizeof(connection_pool_stats_t));
    pthread_mutex_unlock(&pool->stats_mutex);
}

// 打印连接池统计信息
void connection_pool_print_stats(connection_pool_t *pool) {
    if (!pool) {
        return;
    }
    
    connection_pool_stats_t stats;
    connection_pool_get_stats(pool, &stats);
    
    log_info("=== 连接池统计信息 ===");
    log_info("总连接数: %d", atomic_load(&stats.total_connections));
    log_info("活跃连接数: %d", atomic_load(&stats.active_connections));
    log_info("空闲连接数: %d", atomic_load(&stats.idle_connections));
    log_info("复用连接数: %d", atomic_load(&stats.reused_connections));
    log_info("新建连接数: %d", atomic_load(&stats.created_connections));
    log_info("关闭连接数: %d", atomic_load(&stats.closed_connections));
    log_info("超时连接数: %d", atomic_load(&stats.timeout_connections));
    log_info("总请求数: %lu", stats.total_requests);
    log_info("总读取字节数: %lu", stats.total_bytes_read);
    log_info("总写入字节数: %lu", stats.total_bytes_written);
    log_info("平均连接生命周期: %.2f秒", stats.avg_connection_lifetime);
    log_info("平均每连接请求数: %.2f", stats.avg_requests_per_conn);
    log_info("======================");
}

// 清理空闲连接
int connection_pool_cleanup_idle(connection_pool_t *pool) {
    if (!pool) {
        return 0;
    }
    
    int cleaned = 0;
    time_t now = time(NULL);
    
    pthread_mutex_lock(&pool->idle_mutex);
    
    for (int i = pool->idle_count - 1; i >= 0; i--) {
        connection_t *conn = pool->idle_connections[i];
        if (conn && (now - 0) > pool->config.idle_timeout) { // 暂时简化，使用固定时间
            // 移除超时的空闲连接
            for (int j = i; j < pool->idle_count - 1; j++) {
                pool->idle_connections[j] = pool->idle_connections[j + 1];
            }
            pool->idle_connections[--pool->idle_count] = NULL;
            
            // 关闭连接
            connection_pool_close_connection(pool, conn);
            cleaned++;
            
            log_debug("清理超时空闲连接：conn=%p, 空闲时间=%ld秒", 
                      conn, now - 0);
        }
    }
    
    pthread_mutex_unlock(&pool->idle_mutex);
    
    if (cleaned > 0) {
        log_info("连接池清理完成：清理了 %d 个超时空闲连接", cleaned);
    }
    
    return cleaned;
}

// 设置连接池配置
int connection_pool_set_config(connection_pool_t *pool, const connection_pool_config_t *config) {
    if (!pool || !config) {
        return -1;
    }
    
    pthread_mutex_lock(&pool->pool_mutex);
    memcpy(&pool->config, config, sizeof(connection_pool_config_t));
    pthread_mutex_unlock(&pool->pool_mutex);
    
    log_info("连接池配置已更新");
    return 0;
}

// 获取连接池配置
void connection_pool_get_config(connection_pool_t *pool, connection_pool_config_t *config) {
    if (!pool || !config) {
        return;
    }
    
    pthread_mutex_lock(&pool->pool_mutex);
    memcpy(config, &pool->config, sizeof(connection_pool_config_t));
    pthread_mutex_unlock(&pool->pool_mutex);
}

// 从Config file中加载连接池配置
connection_pool_config_t connection_pool_load_config(const config_t *config) {
    connection_pool_config_t pool_config = {0};
    
    if (!config) {
        // 使用默认配置
        pool_config.max_connections = 10000;
        pool_config.min_idle_connections = 10;
        pool_config.max_idle_connections = 1000;
        pool_config.connection_timeout = 30;
        pool_config.idle_timeout = 60;
        pool_config.keepalive_timeout = 30;
        pool_config.max_requests_per_conn = 1000;
        pool_config.enable_connection_reuse = 1;
        pool_config.enable_connection_pooling = 1;
        pool_config.pool_cleanup_interval = 30;
        return pool_config;
    }
    
    // 从服务器配置中加载
    pool_config.max_connections = config->max_connections;
    pool_config.min_idle_connections = config->worker_connections / 10; // 10%作为最小空闲
    pool_config.max_idle_connections = config->worker_connections / 2;  // 50%作为最大空闲
    pool_config.connection_timeout = config->connection_timeout;
    pool_config.idle_timeout = config->keepalive_timeout * 2; // 空闲超时是keepalive的2倍
    pool_config.keepalive_timeout = config->keepalive_timeout;
    pool_config.max_requests_per_conn = 1000; // 默认值
    pool_config.enable_connection_reuse = 1;   // 默认启用
    pool_config.enable_connection_pooling = 1; // 默认启用
    pool_config.pool_cleanup_interval = 30;    // 30秒清理一次
    
    return pool_config;
} 
/**
 * 配置验证和优化模块实现
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/resource.h>

#include "../include/config.h"
#include "../include/config_defaults.h"
#include "../include/error_handling.h"
#include "../include/logger.h"

// 验证并优化Worker processes配置
static int validate_worker_processes(config_t *config) {
    CHECK_NULL_RETURN(config, -1);
    
    // 获取系统CPU核心数
    long cpu_count = sysconf(_SC_NPROCESSORS_ONLN);
    if (cpu_count <= 0) {
        cpu_count = DEFAULT_WORKER_PROCESSES_FALLBACK;
    }
    
    // 如果配置为0或负数，使用CPU核心数
    if (config->worker_processes <= 0) {
        config->worker_processes = (int)cpu_count;
        log_info("Auto-set Worker processes to CPU core count: %d", config->worker_processes);
    }
    
    // 验证范围
    CHECK_BOUNDS(config->worker_processes, 1, 64, -1);
    
    // 优化建议：Worker processes不应超过CPU核心数的2倍
    if (config->worker_processes > cpu_count * 2) {
        log_warn("Worker processes(%d) exceeds 2x CPU core count(%ld), may affect performance", 
                 config->worker_processes, cpu_count * 2);
    }
    
    return 0;
}

// 验证并优化连接数配置
static int validate_connection_config(config_t *config) {
    CHECK_NULL_RETURN(config, -1);
    
    // 验证Worker连接数
    if (config->worker_connections <= 0) {
        config->worker_connections = DEFAULT_WORKER_CONNECTIONS;
        log_info("Using default Worker connections: %d", config->worker_connections);
    }
    
    CHECK_BOUNDS(config->worker_connections, 1, 65536, -1);
    
    // 自动计算最大连接数
    int calculated_max = config->worker_processes * config->worker_connections;
    if (config->max_connections != calculated_max) {
        log_info("Adjust max connections from %d to %d (worker_processes * worker_connections)", 
                 config->max_connections, calculated_max);
        config->max_connections = calculated_max;
    }
    
    // 检查系统文件描述符限制
    struct rlimit rlim;
    if (getrlimit(RLIMIT_NOFILE, &rlim) == 0) {
        if (config->worker_rlimit_nofile > (int)rlim.rlim_max) {
            log_warn("Configured file descriptor limit(%d) exceeds system hard limit(%ld), will adjust to system limit", 
                     config->worker_rlimit_nofile, (long)rlim.rlim_max);
            config->worker_rlimit_nofile = (int)rlim.rlim_max;
        }
        
        // 确保文件描述符限制足够支持最大连接数
        int required_fds = config->max_connections + 1000; // 额外1000个用于其他用途
        if (config->worker_rlimit_nofile < required_fds) {
            log_warn("File descriptor limit(%d) may not support max connections(%d), recommend at least %d", 
                     config->worker_rlimit_nofile, config->max_connections, required_fds);
        }
    }
    
    return 0;
}

// 验证并优化内存配置
static int validate_memory_config(config_t *config) {
    CHECK_NULL_RETURN(config, -1);
    
    // 验证内存池大小
    if (config->memory_pool_size <= 0) {
        config->memory_pool_size = DEFAULT_MEMORY_POOL_SIZE;
        log_info("Using default memory pool size: %zu bytes", config->memory_pool_size);
    }
    
    // 检查系统可用内存
    long page_size = sysconf(_SC_PAGESIZE);
    long num_pages = sysconf(_SC_PHYS_PAGES);
    if (page_size > 0 && num_pages > 0) {
        size_t total_memory = (size_t)page_size * (size_t)num_pages;
        size_t total_pool_memory = config->memory_pool_size * config->worker_processes;
        
        if (total_pool_memory > total_memory / 2) {
            log_warn("Total memory pool size(%zu MB) exceeds 50%% of system memory(%zu MB), may cause memory shortage", 
                     total_pool_memory / (1024 * 1024), total_memory / (1024 * 1024) / 2);
        }
    }
    
    // 验证内存块大小
    if (config->memory_block_size <= 0) {
        config->memory_block_size = DEFAULT_MEMORY_BLOCK_SIZE;
    }
    
    // 内存块大小应该是页大小的倍数
    if (page_size > 0 && config->memory_block_size % page_size != 0) {
        size_t aligned_size = ((config->memory_block_size + page_size - 1) / page_size) * page_size;
        log_info("Adjust memory block size from %zu to %zu (page aligned)", config->memory_block_size, aligned_size);
        config->memory_block_size = aligned_size;
    }
    
    return 0;
}

// 验证并优化超时配置
static int validate_timeout_config(config_t *config) {
    CHECK_NULL_RETURN(config, -1);
    
    // 验证Keep-alive超时
    CHECK_BOUNDS(config->keepalive_timeout, 0, 3600, -1);
    
    // 验证各种超时设置
    if (config->client_header_timeout <= 0) {
        config->client_header_timeout = DEFAULT_CLIENT_HEADER_TIMEOUT;
    }
    CHECK_BOUNDS(config->client_header_timeout, 1, 300, -1);
    
    if (config->client_body_timeout <= 0) {
        config->client_body_timeout = DEFAULT_CLIENT_BODY_TIMEOUT;
    }
    CHECK_BOUNDS(config->client_body_timeout, 1, 300, -1);
    
    if (config->send_timeout <= 0) {
        config->send_timeout = DEFAULT_SEND_TIMEOUT;
    }
    CHECK_BOUNDS(config->send_timeout, 1, 300, -1);
    
    // proxy超时配置
    if (config->proxy_connect_timeout <= 0) {
        config->proxy_connect_timeout = DEFAULT_PROXY_CONNECT_TIMEOUT;
    }
    if (config->proxy_send_timeout <= 0) {
        config->proxy_send_timeout = DEFAULT_PROXY_SEND_TIMEOUT;
    }
    if (config->proxy_read_timeout <= 0) {
        config->proxy_read_timeout = DEFAULT_PROXY_READ_TIMEOUT;
    }
    
    return 0;
}

// 验证并优化缓冲区配置
static int validate_buffer_config(config_t *config) {
    CHECK_NULL_RETURN(config, -1);
    
    // 验证客户端头部缓冲区
    if (config->client_header_buffer_size <= 0) {
        config->client_header_buffer_size = DEFAULT_CLIENT_HEADER_BUFFER_SIZE;
    }
    
    // 缓冲区大小应该合理
    CHECK_BOUNDS(config->client_header_buffer_size, 1024, 1024*1024, -1); // 1KB - 1MB
    
    // 验证大头部缓冲区
    if (config->large_client_header_buffers <= 0) {
        config->large_client_header_buffers = DEFAULT_LARGE_CLIENT_HEADER_BUFFERS;
    }
    
    // 验证请求体缓冲区
    if (config->client_body_buffer_size <= 0) {
        config->client_body_buffer_size = DEFAULT_CLIENT_BODY_BUFFER_SIZE;
    }
    CHECK_BOUNDS(config->client_body_buffer_size, 1024, 10*1024*1024, -1); // 1KB - 10MB
    
    // 验证最大请求大小
    if (config->max_request_size <= 0) {
        config->max_request_size = DEFAULT_MAX_REQUEST_SIZE;
    }
    
    // 最大请求大小不应该太大，防止DoS攻击
    if (config->max_request_size > 100 * 1024 * 1024) { // 100MB
        log_warn("Max request size(%zu MB) too large, may pose DoS risk", 
                 config->max_request_size / (1024 * 1024));
    }
    
    return 0;
}

// 验证路由配置
static int validate_routes_config(config_t *config) {
    CHECK_NULL_RETURN(config, -1);
    
    if (config->route_count <= 0) {
        log_error("No routes configured");
        return -1;
    }
    
    if (config->route_count > MAX_ROUTES) {
        log_error("Route count(%d)超过最大限制(%d)", config->route_count, MAX_ROUTES);
        return -1;
    }
    
    for (int i = 0; i < config->route_count; i++) {
        route_t *route = &config->routes[i];
        
        // 验证路径前缀
        if (strlen(route->path_prefix) == 0) {
            log_error("Route %d path prefix is empty", i + 1);
            return -1;
        }
        
        // 验证路由类型特定的配置
        if (route->type == ROUTE_PROXY) {
            if (strlen(route->target_host) == 0) {
                log_error("Route %d target host is empty", i + 1);
                return -1;
            }
            CHECK_BOUNDS(route->target_port, 1, 65535, -1);
        } else if (route->type == ROUTE_STATIC) {
            if (strlen(route->local_path) == 0) {
                log_error("Route %d local path is empty", i + 1);
                return -1;
            }
            
            // 检查本地路径是否存在
            struct stat st;
            if (stat(route->local_path, &st) != 0) {
                log_warn("Route %d local path does not exist: %s", i + 1, route->local_path);
            } else if (!S_ISDIR(st.st_mode)) {
                log_warn("Route %d local path is not a directory: %s", i + 1, route->local_path);
            }
        }
        
        // 验证字符集
        if (strlen(route->charset) == 0) {
            strncpy(route->charset, DEFAULT_ROUTE_CHARSET, sizeof(route->charset) - 1);
            route->charset[sizeof(route->charset) - 1] = '\0';
        }
    }
    
    return 0;
}

// 验证日志配置
static int validate_log_config(config_t *config) {
    CHECK_NULL_RETURN(config, -1);
    
    // 验证日志路径
    if (strlen(config->log_config.log_path) == 0) {
        strncpy(config->log_config.log_path, DEFAULT_LOG_PATH, 
                sizeof(config->log_config.log_path) - 1);
        config->log_config.log_path[sizeof(config->log_config.log_path) - 1] = '\0';
        log_info("Using default log path: %s", config->log_config.log_path);
    }
    
    // 检查日志目录是否存在，如果不存在则创建
    struct stat st;
    if (stat(config->log_config.log_path, &st) != 0) {
        if (mkdir(config->log_config.log_path, 0755) != 0) {
            log_error("Unable to create log directory: %s", config->log_config.log_path);
            return -1;
        }
        log_info("Created log directory: %s", config->log_config.log_path);
    } else if (!S_ISDIR(st.st_mode)) {
        log_error("Log path is not a directory: %s", config->log_config.log_path);
        return -1;
    }
    
    // 验证日志级别
    CHECK_BOUNDS(config->log_config.log_level, LOG_LEVEL_DEBUG, LOG_LEVEL_ERROR, -1);
    
    return 0;
}

// 主配置验证函数
int validate_and_optimize_config(config_t *config) {
    CHECK_NULL_RETURN(config, -1);
    
    log_info("Starting configuration validation and optimization...");
    
    // 验证各个配置模块
    if (validate_worker_processes(config) != 0) {
        log_error("Worker process configuration validation failed");
        return -1;
    }
    
    if (validate_connection_config(config) != 0) {
        log_error("Connection configuration validation failed");
        return -1;
    }
    
    if (validate_memory_config(config) != 0) {
        log_error("Memory configuration validation failed");
        return -1;
    }
    
    if (validate_timeout_config(config) != 0) {
        log_error("Timeout configuration validation failed");
        return -1;
    }
    
    if (validate_buffer_config(config) != 0) {
        log_error("Buffer configuration validation failed");
        return -1;
    }
    
    if (validate_routes_config(config) != 0) {
        log_error("Route configuration validation failed");
        return -1;
    }
    
    if (validate_log_config(config) != 0) {
        log_error("Log configuration validation failed");
        return -1;
    }
    
    log_info("Configuration validation and optimization completed");
    return 0;
}

// 打印配置摘要
void print_config_summary(const config_t *config) {
    if (!config) return;
    
    log_info("=== Configuration Summary ===");
    log_info("Worker processes: %d", config->worker_processes);
    log_info("Connections per Worker: %d", config->worker_connections);
    log_info("Max total connections: %d", config->max_connections);
    log_info("Listening port: %d", config->listen_port);
    log_info("Keep-alive timeout: %d seconds", config->keepalive_timeout);
    log_info("Max request size: %.1f MB", (double)config->max_request_size / (1024 * 1024));
    log_info("Memory pool size: %.1f MB", (double)config->memory_pool_size / (1024 * 1024));
    log_info("File descriptor limit: %d", config->worker_rlimit_nofile);
    log_info("Route count: %d", config->route_count);
    log_info("Log level: %d", config->log_config.log_level);
    log_info("===============");
}
/**
 * Worker process管理模块实现
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <stdatomic.h>
#include <errno.h>
#include <fcntl.h>

#include "../include/worker_process.h"
#include "../include/event_loop.h"
#include "../include/event_loop.h"
#include "../include/connection.h"
#include "../include/connection_pool.h"
#include "../include/logger.h"
#include "../include/config.h"
#include "../include/connection_limit.h"
#include "../include/process_title.h"
#include "../include/shared_memory.h"
#include "../include/file_io_enhanced.h"

// 全局Worker上下文
static worker_context_t *g_worker_ctx = NULL;

// 全局连接池
static connection_pool_t *g_connection_pool = NULL;

// 信号处理标志
static volatile sig_atomic_t g_reload_config = 0;
static volatile sig_atomic_t g_shutdown_worker = 0;
static volatile sig_atomic_t g_terminate_worker = 0;

// 前向声明
static void worker_signal_handler(int sig);
static int setup_worker_signals(void);
static void worker_accept_callback(int listen_fd, void *arg);

/**
 * Worker process信号处理器
 */
static void worker_signal_handler(int sig) {
    switch (sig) {
        case SIGHUP:
            g_reload_config = 1;
            log_info("Worker process %d 接收到SIGHUP信号，准备reload configuration", getpid());
            break;
            
        case SIGTERM:
            g_shutdown_worker = 1;
            log_info("Worker process %d received SIGTERM signal, preparing for graceful shutdown", getpid());
            break;
            
        case SIGQUIT:
            g_terminate_worker = 1;
            log_info("Worker process %d received SIGQUIT signal, preparing to terminate immediately", getpid());
            break;
            
        default:
            log_warn("Worker process %d received unhandled signal: %d", getpid(), sig);
            break;
    }
}

/**
 * 设置Worker process信号处理
 */
static int setup_worker_signals(void) {
    struct sigaction sa;
    
    // Set up signal handler
    sa.sa_handler = worker_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    
    if (sigaction(SIGHUP, &sa, NULL) == -1 ||
        sigaction(SIGTERM, &sa, NULL) == -1 ||
        sigaction(SIGQUIT, &sa, NULL) == -1) {
        log_error("设置Worker process信号处理器失败: %s", strerror(errno));
        return -1;
    }
    
    // Ignore SIGPIPE signal
    signal(SIGPIPE, SIG_IGN);
    
    return 0;
}

/**
 * Worker process接受连接回调函数
 */
static void worker_accept_callback(int listen_fd, void *arg) {
    (void)arg; // avoid unused parameter warning
    
    // Batch accept connections to improve performance under high concurrency
    int accepted_count = 0;
    const int max_accept_per_loop = 100; // maximum connections per loop
    
    while (accepted_count < max_accept_per_loop) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        // Accept new connection
        int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // no new connections, this is normal
                break;
            }
            log_error("Worker process %d Accept connection failed: %s", getpid(), strerror(errno));
            break;
        }
    
    // Set non-blocking mode
    int flags = fcntl(client_fd, F_GETFL, 0);
    if (flags < 0 || fcntl(client_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        log_error("Failed to set client socket non-blocking mode: %s", strerror(errno));
        close(client_fd);
        continue;
    }
    
    // Get connection object from connection pool
    connection_t *conn = NULL;
    if (g_connection_pool) {
        // Use connection pool
        conn = connection_pool_get_connection(g_connection_pool, client_fd, 
                                            g_worker_ctx->event_loop, 1, // Use unified event loop
                                            g_worker_ctx->config, &client_addr);
    } else {
        // Create connection directly (compatibility mode)
        conn = connection_create(client_fd, g_worker_ctx->event_loop, g_worker_ctx->config, &client_addr);
    }
    
    if (conn == NULL) {
        log_error("Worker process %d Failed to create connection", getpid());
        close(client_fd);
        continue;
    }
    
        // Update statistics - use atomic operations
        atomic_fetch_add(&g_worker_ctx->active_connections, 1);
        atomic_fetch_add(&g_worker_ctx->total_connections, 1);
        
        accepted_count++;
        
        // Thread-safe IP address conversion
        char client_ip[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN) == NULL) {
            strcpy(client_ip, "0.0.0.0");
        }
        log_debug("Worker process %d Accept new connection: %s:%d", 
                  getpid(), client_ip, ntohs(client_addr.sin_port));
    }
    
    if (accepted_count > 0) {
        log_debug("Worker process %d Batch accept connections completed, accepted connections", getpid(), accepted_count);
    }
}

/**
 * Unified event loop accept connection callback adapter
 */
static void unified_worker_accept_callback(int listen_fd, void *arg) {
    worker_accept_callback(listen_fd, arg);
}

/**
 * Unified connection callback adapter
 */
void unified_connection_callback(int client_fd, void *arg) {
    connection_t *conn = (connection_t *)arg;
    if (!conn) {
        return;
    }
    // Unified event loop will call corresponding callback based on event type
    connection_read_callback(client_fd, (void *)conn);
}

/**
 * 重新加载Worker process配置
 */
static int worker_reload_config(void) {
    log_info("Worker process %d 开始reload configuration", getpid());
    
    g_worker_ctx->state = WORKER_RELOADING;
    
    // 从共享内存获取新配置
    config_t *new_config = get_shared_config();
    if (new_config == NULL) {
        log_error("Worker process %d Failed to get configuration from shared memory", getpid());
        g_worker_ctx->state = WORKER_RUNNING;
        return -1;
    }
    
    // 更新配置
    if (g_worker_ctx->config != NULL) {
        free_config(g_worker_ctx->config);
    }
    g_worker_ctx->config = new_config;
    
    g_worker_ctx->state = WORKER_RUNNING;
    
    log_info("Worker process %d Configuration reload completed", getpid());
    return 0;
}

/**
 * Worker processMain function
 */
int worker_process_run(int worker_id, int listen_fd, config_t *config) {
    // 分配Worker上下文
    g_worker_ctx = (worker_context_t *)malloc(sizeof(worker_context_t));
    if (g_worker_ctx == NULL) {
        log_error("分配Worker process上下文内存失败");
        return -1;
    }
    
    memset(g_worker_ctx, 0, sizeof(worker_context_t));
    
    // 初始化Worker上下文
    g_worker_ctx->worker_id = worker_id;
    g_worker_ctx->worker_pid = getpid();
    g_worker_ctx->state = WORKER_STARTING;
    g_worker_ctx->listen_fd = listen_fd;
    g_worker_ctx->start_time = time(NULL);
    
    // 初始化原子变量
    atomic_init(&g_worker_ctx->requests_processed, 0);
    atomic_init(&g_worker_ctx->bytes_sent, 0);
    atomic_init(&g_worker_ctx->bytes_received, 0);
    atomic_init(&g_worker_ctx->active_connections, 0);
    atomic_init(&g_worker_ctx->total_connections, 0);
    
    // 初始化统计互斥锁
    if (pthread_mutex_init(&g_worker_ctx->stats_mutex, NULL) != 0) {
        log_error("Worker process %d Failed to initialize statistics mutex", getpid());
        free(g_worker_ctx);
        return -1;
    }
    
    // 复制配置
    g_worker_ctx->config = duplicate_config(config);
    if (g_worker_ctx->config == NULL) {
        log_error("Worker process %d Failed to copy configuration", getpid());
        free(g_worker_ctx);
        return -1;
    }
    
    // 设置信号处理
    if (setup_worker_signals() != 0) {
        free_config(g_worker_ctx->config);
        free(g_worker_ctx);
        return -1;
    }
    
    // Worker process环境变量应该已经在fork时设置，这里再次确认
    char worker_env[32];
    snprintf(worker_env, sizeof(worker_env), "%d", worker_id);
    if (getenv("WORKER_PROCESS_ID") == NULL) {
        setenv("WORKER_PROCESS_ID", worker_env, 1);
    }
    
    // Worker process继承Master进程的日志系统配置，无需重新初始化
    // Just need to simply record startup information
    log_info("Worker process %d 启动，PID: %d", worker_id, getpid());
    
    // Initialize connection management module - use memory pool size from configuration
    size_t pool_size = g_worker_ctx->config->memory_pool_size > 0 ? 
                      g_worker_ctx->config->memory_pool_size : 1024 * 1024 * 100; // 默认100MB
    if (init_connection_manager(pool_size) != 0) {
        log_error("Worker process %d Failed to initialize connection management module", getpid());
        free_config(g_worker_ctx->config);
        free(g_worker_ctx);
        return -1;
    }
    
    // 初始化连接池
    connection_pool_config_t pool_config = connection_pool_load_config(g_worker_ctx->config);
    g_connection_pool = connection_pool_create(&pool_config);
    if (g_connection_pool == NULL) {
        log_warn("Worker process %d Failed to create connection pool, will use direct connection creation mode", getpid());
    } else {
        log_info("Worker process %d Connection pool initialization successful", getpid());
    }
    
    // Update connection limit configuration
    update_connection_limit_from_config(
        g_worker_ctx->config->connection_limit_per_ip,
        g_worker_ctx->config->connection_limit_window
    );
    
    // Initialize enhanced file I/O module
    file_io_config_t file_io_config = {
        .cache_size = 100,                    // 100MB缓存
        .max_file_size = 50,                  // 最大50MB文件
        .enable_mmap = 1,                     // 启用mmap
        .enable_async = 0,                    // 暂时禁用异步I/O
        .enable_sendfile = 1,                 // 启用sendfile
        .cache_cleanup_interval = 300,        // 5分钟清理间隔
        .read_buffer_size = 8192,             // 8KB读取缓冲区
        .write_buffer_size = 8192             // 8KB写入缓冲区
    };
    
    if (file_io_enhanced_init(&file_io_config) != 0) {
        log_warn("Worker process %d Initialize enhanced file I/O module失败，将使用标准文件处理", getpid());
    } else {
        log_info("Worker process %d Enhanced file I/O module initialization successful", getpid());
    }
    
    // Use unified high-performance event loop
    int max_events = g_worker_ctx->config->event_loop_max_events > 0 ? 
                    g_worker_ctx->config->event_loop_max_events : 1000;
    
    g_worker_ctx->event_loop = event_loop_create(max_events);
    if (g_worker_ctx->event_loop == NULL) {
        log_error("Worker process %d Failed to create unified event loop", getpid());
        cleanup_connection_manager();
        free_config(g_worker_ctx->config);
        free(g_worker_ctx);
        return -1;
    }
    
    // 将监听套接字添加到统一事件循环
    if (event_loop_add_handler(g_worker_ctx->event_loop, listen_fd, EVENT_READ, 
                              unified_worker_accept_callback, NULL, NULL) != 0) {
        log_error("Worker process %d Failed to add listen socket to unified event loop", getpid());
        event_loop_destroy(g_worker_ctx->event_loop);
        cleanup_connection_manager();
        free_config(g_worker_ctx->config);
        free(g_worker_ctx);
        return -1;
    }
    
    // 启动统一事件循环
    if (event_loop_start(g_worker_ctx->event_loop) != 0) {
        log_error("Worker process %d Failed to start unified event loop", getpid());
        event_loop_destroy(g_worker_ctx->event_loop);
        cleanup_connection_manager();
        free_config(g_worker_ctx->config);
        free(g_worker_ctx);
        return -1;
    }
    
    g_worker_ctx->state = WORKER_RUNNING;
    
    // 设置Worker process标题
    setproctitle("x-server: worker process");
    
    log_info("Worker process %d Start running", getpid());
    
    // Memory cleanup counter
    int memory_cleanup_counter = 0;
    const int MEMORY_CLEANUP_INTERVAL = 1000; // clean up memory every loops
    
    // Worker process主循环
    while (g_worker_ctx->state != WORKER_STOPPED) {
        // Check signal flags
        if (g_reload_config) {
            g_reload_config = 0;
            worker_reload_config();
        }
        
        if (g_shutdown_worker) {
            g_shutdown_worker = 0;
            log_info("Worker process %d Start graceful shutdown", getpid());
            worker_graceful_shutdown();
            break;
        }
        
        if (g_terminate_worker) {
            g_terminate_worker = 0;
            log_info("Worker process %d Immediately terminate", getpid());
            g_worker_ctx->state = WORKER_STOPPED;
            break;
        }
        
        // Periodic memory cleanup
        memory_cleanup_counter++;
        if (memory_cleanup_counter >= MEMORY_CLEANUP_INTERVAL) {
            // Force compress memory pool - cleanup through connection manager
            extern int compress_connection_pool(void);
            int freed_blocks = compress_connection_pool();
            if (freed_blocks > 0) {
                log_info("Worker process %d Periodic memory cleanup完成，释放了 %d 个内存块", getpid(), freed_blocks);
            }
            memory_cleanup_counter = 0;
        }
        
        // Check and flush idle log buffers
        logger_check_idle_flush();
        
        // Sleep for a short time to let event loop process events
        usleep(10000); // 10ms，减少延迟
    }
    
    // Stop event loop
    event_loop_stop(g_worker_ctx->event_loop);
    
    // Clean up resources
    event_loop_destroy(g_worker_ctx->event_loop);
    
    // Destroy connection pool
    if (g_connection_pool) {
        connection_pool_print_stats(g_connection_pool);
        connection_pool_destroy(g_connection_pool);
        g_connection_pool = NULL;
    }
    
    // Destroy enhanced file I/O module
    file_io_enhanced_destroy();
    
    cleanup_connection_manager();
    free_config(g_worker_ctx->config);
    
    log_info("Worker process %d Exit, processed requests, sent bytes: %lu", 
             getpid(), atomic_load(&g_worker_ctx->requests_processed), 
             atomic_load(&g_worker_ctx->bytes_sent));
    
    // Clean up mutex
    pthread_mutex_destroy(&g_worker_ctx->stats_mutex);
    
    free(g_worker_ctx);
    g_worker_ctx = NULL;
    
    return 0;
}

/**
 * 获取Worker process上下文
 */
worker_context_t *get_worker_context(void) {
    return g_worker_ctx;
}

/**
 * Worker process优雅关闭
 */
void worker_graceful_shutdown(void) {
    if (g_worker_ctx == NULL) {
        return;
    }
    
    g_worker_ctx->state = WORKER_STOPPING;
    
    // 停止Accept new connection
    event_loop_del_handler(g_worker_ctx->event_loop, g_worker_ctx->listen_fd);
    
    // Wait for existing connections to complete processing (wait up to seconds)
    time_t start_time = time(NULL);
    while (g_worker_ctx->active_connections > 0 && 
           (time(NULL) - start_time) < 30) {
        sleep(1);
    }
    
    if (g_worker_ctx->active_connections > 0) {
        log_warn("Worker process %d Still have active connections, force close", 
                 getpid(), g_worker_ctx->active_connections);
    }
    
    g_worker_ctx->state = WORKER_STOPPED;
}

/**
 * Thread-safe statistics update
 */
void update_worker_stats_safe(size_t bytes_sent, size_t bytes_received) {
    if (g_worker_ctx == NULL) {
        return;
    }
    
    atomic_fetch_add(&g_worker_ctx->bytes_sent, bytes_sent);
    atomic_fetch_add(&g_worker_ctx->bytes_received, bytes_received);
    atomic_fetch_add(&g_worker_ctx->total_requests, 1);
}

/**
 * Safely increment connection count
 */
void increment_connection_count_safe(void) {
    if (g_worker_ctx == NULL) {
        return;
    }
    
    atomic_fetch_add(&g_worker_ctx->active_connections, 1);
    atomic_fetch_add(&g_worker_ctx->total_connections, 1);
}

/**
 * Safely decrement connection count
 */
void decrement_connection_count_safe(void) {
    if (g_worker_ctx == NULL) {
        return;
    }
    
    atomic_fetch_sub(&g_worker_ctx->active_connections, 1);
}

/**
 * 获取Worker process的连接池
 */
connection_pool_t *get_worker_connection_pool(void) {
    return g_connection_pool;
}

/**
 * 内部函数：获取Worker process的连接池
 */
connection_pool_t *get_worker_connection_pool_internal(void) {
    return g_connection_pool;
}

/**
 * 配置文件解析模块头文件 - 简化版
 * 只支持新格式的gateway_multiprocess.conf配置文件
 */

#ifndef CONFIG_H
#define CONFIG_H

#include <time.h>

#define MAX_ROUTES 64
#define MAX_PATH_LEN 512
#define MAX_HOST_LEN 256
#define MAX_CHARSET_LEN 32
#define MAX_PATH_PREFIX_LEN 256
#define MAX_LOCAL_PATH_LEN 512
#define MAX_LOG_PATH_LEN 256

// 路由类型
typedef enum {
    ROUTE_STATIC,   // 静态文件路由
    ROUTE_PROXY     // 代理路由
} route_type_t;

// 认证类型
typedef enum {
    AUTH_NONE,      // 无认证
    AUTH_OAUTH      // OAuth认证
} auth_type_t;

// 路由配置结构体
typedef struct {
    route_type_t type;
    char path_prefix[MAX_PATH_LEN];     // 路径前缀
    char target_host[MAX_HOST_LEN];     // 目标主机（代理模式）
    int target_port;                    // 目标端口（代理模式）
    char local_path[MAX_PATH_LEN];      // 本地路径（静态文件模式）
    char charset[MAX_CHARSET_LEN];      // 字符集
    auth_type_t auth_type;              // 认证类型
} route_t;

// 日志配置结构体
typedef struct {
    char log_path[MAX_PATH_LEN];        // 日志文件路径
    int log_daily;                      // 是否按日分割日志
    int log_level;                      // 日志级别
} log_config_t;

// 主配置结构体
typedef struct {
    int worker_processes;               // Worker进程数量
    int listen_port;                    // 监听端口
    int max_connections;                // 最大连接数
    int keepalive_timeout;              // Keep-alive超时时间
    int client_max_body_size;           // 客户端最大请求体大小
    size_t max_request_size;            // 最大请求大小
    
    // 路由配置
    route_t routes[MAX_ROUTES];         // 路由数组（固定大小，适合共享内存）
    int route_count;                    // 路由数量
    
    // 日志配置
    log_config_t log_config;
    
    // 性能配置
    int worker_connections;             // 每个Worker的最大连接数
    int worker_rlimit_nofile;           // Worker进程文件描述符限制
    int tcp_nodelay;                    // TCP_NODELAY选项
    int tcp_nopush;                     // TCP_NOPUSH选项
    int tcp_fastopen;                   // TCP Fast Open选项
    int reuseport;                      // 端口重用选项
    
    // 缓冲区配置
    int client_header_buffer_size;      // 客户端请求头缓冲区大小
    int large_client_header_buffers;    // 大请求头缓冲区数量
    int client_body_buffer_size;        // 客户端请求体缓冲区大小
    
    // 超时配置
    int client_header_timeout;          // 客户端请求头超时
    int client_body_timeout;            // 客户端请求体超时
    int send_timeout;                   // 发送超时
    
    // 代理配置
    int proxy_connect_timeout;          // 代理连接超时
    int proxy_send_timeout;             // 代理发送超时
    int proxy_read_timeout;             // 代理读取超时
    int proxy_buffer_size;              // 代理缓冲区大小
    int proxy_buffers;                  // 代理缓冲区数量
    int proxy_busy_buffers_size;        // 代理忙缓冲区大小
    
    // 10K并发优化配置
    int event_loop_max_events;          // 事件循环最大事件数
    int event_loop_timeout;             // 事件循环超时时间（毫秒）
    int event_loop_batch_size;          // 事件批处理大小
    size_t memory_pool_size;            // 内存池大小
    int memory_block_size;              // 内存块大小
    int memory_pool_segments;           // 内存池分段数
    int memory_pool_cleanup_interval;   // 内存池清理间隔（秒）
    int connection_limit_per_ip;        // 每个IP的连接限制
    int connection_limit_window;        // 连接限制时间窗口（秒）
    int connection_timeout;             // 连接超时时间（秒）
    int connection_keepalive_max;       // 最大Keep-alive连接数
    int use_thread_pool;                // 是否启用线程池
    int thread_pool_size;               // 线程池大小
    int thread_pool_queue_size;         // 线程池队列大小
} config_t;

/**
 * 加载配置文件（只支持新格式）
 * 
 * @param filename 配置文件路径
 * @return 配置结构体指针，失败返回NULL
 */
config_t *load_config(const char *filename);

/**
 * 释放配置结构体
 * 
 * @param config 配置结构体指针
 */
void free_config(config_t *config);

/**
 * 查找匹配的路由
 * 
 * @param config 配置结构体指针
 * @param path 请求路径
 * @return 匹配的路由指针，未找到返回NULL
 */
route_t *find_route(config_t *config, const char *path);

/**
 * 验证配置的有效性
 * 
 * @param config 配置结构体指针
 * @return 有效返回1，无效返回0
 */
int validate_config(config_t *config);

/**
 * 获取默认配置
 * 
 * @return 默认配置结构体指针
 */
config_t *get_default_config(void);

/**
 * 复制配置结构体（用于多进程）
 * 
 * @param config 源配置结构体指针
 * @return 新的配置结构体指针
 */
config_t *duplicate_config(config_t *config);

#endif /* CONFIG_H */

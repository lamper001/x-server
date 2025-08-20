/**
 * 配置默认值定义 - 统一管理所有默认配置
 */

#ifndef CONFIG_DEFAULTS_H
#define CONFIG_DEFAULTS_H

// 进程配置默认值
#define DEFAULT_WORKER_PROCESSES_AUTO   0  // 自动检测CPU核心数
#define DEFAULT_WORKER_PROCESSES_FALLBACK 14
#define DEFAULT_WORKER_CONNECTIONS      8192
#define DEFAULT_WORKER_RLIMIT_NOFILE    1048576

// 网络配置默认值
#define DEFAULT_LISTEN_PORT             9001
#define DEFAULT_KEEPALIVE_TIMEOUT       30
#define DEFAULT_MAX_REQUEST_SIZE        (50 * 1024 * 1024)  // 50MB
#define DEFAULT_TCP_NODELAY             1
#define DEFAULT_TCP_NOPUSH              0
#define DEFAULT_TCP_FASTOPEN            1
#define DEFAULT_REUSEPORT               1

// 缓冲区配置默认值
#define DEFAULT_CLIENT_HEADER_BUFFER_SIZE    (16 * 1024)   // 16KB
#define DEFAULT_LARGE_CLIENT_HEADER_BUFFERS  (32 * 16 * 1024) // 32个16KB
#define DEFAULT_CLIENT_BODY_BUFFER_SIZE      (1024 * 1024)    // 1MB
#define DEFAULT_PROXY_BUFFER_SIZE            (16 * 1024)      // 16KB
#define DEFAULT_PROXY_BUFFERS                (16 * 16 * 1024) // 16个16KB
#define DEFAULT_PROXY_BUSY_BUFFERS_SIZE      (32 * 1024)     // 32KB

// 超时配置默认值
#define DEFAULT_CLIENT_HEADER_TIMEOUT   30
#define DEFAULT_CLIENT_BODY_TIMEOUT     30
#define DEFAULT_SEND_TIMEOUT            30
#define DEFAULT_PROXY_CONNECT_TIMEOUT   15
#define DEFAULT_PROXY_SEND_TIMEOUT      30
#define DEFAULT_PROXY_READ_TIMEOUT      30

// 线程池配置默认值
#define DEFAULT_USE_THREAD_POOL         1
#define DEFAULT_THREAD_POOL_SIZE        4
#define DEFAULT_THREAD_POOL_QUEUE_SIZE  2000

// 事件循环配置默认值
#define DEFAULT_EVENT_LOOP_MAX_EVENTS   50000
#define DEFAULT_EVENT_LOOP_TIMEOUT      5
#define DEFAULT_EVENT_LOOP_BATCH_SIZE   2000

// 内存池配置默认值
#define DEFAULT_MEMORY_POOL_SIZE        (200 * 1024 * 1024)  // 200MB
#define DEFAULT_MEMORY_BLOCK_SIZE       (32 * 1024)          // 32KB
#define DEFAULT_MEMORY_POOL_SEGMENTS    32
#define DEFAULT_MEMORY_POOL_CLEANUP_INTERVAL 300

// 连接限制配置默认值
#define DEFAULT_CONNECTION_LIMIT_PER_IP     1000
#define DEFAULT_CONNECTION_LIMIT_WINDOW     60
#define DEFAULT_CONNECTION_TIMEOUT          300
#define DEFAULT_CONNECTION_KEEPALIVE_MAX    5000

// 日志配置默认值
#define DEFAULT_LOG_PATH                "./logs"
#define DEFAULT_LOG_DAILY               1
#define DEFAULT_LOG_LEVEL               LOG_LEVEL_WARN

// 路由配置默认值
#define DEFAULT_ROUTE_PATH_PREFIX       "/"
#define DEFAULT_ROUTE_LOCAL_PATH        "./public"
#define DEFAULT_ROUTE_CHARSET           "utf-8"

// 性能优化宏
#define CALCULATE_MAX_CONNECTIONS(workers, connections) ((workers) * (connections))
#define CALCULATE_TOTAL_MEMORY_POOL(workers, pool_size) ((workers) * (pool_size))

// 配置验证宏
#define VALIDATE_RANGE(value, min, max) ((value) >= (min) && (value) <= (max))
#define VALIDATE_POSITIVE(value) ((value) > 0)

#endif // CONFIG_DEFAULTS_H
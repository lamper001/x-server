/**
 * 安全增强模块 - 提供统一的安全检查和防护功能
 */

#ifndef SECURITY_ENHANCED_H
#define SECURITY_ENHANCED_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

// 安全检查结果
typedef enum {
    SECURITY_CHECK_PASS = 0,        // 通过
    SECURITY_CHECK_FAIL = -1,       // 失败
    SECURITY_CHECK_SUSPICIOUS = -2,  // 可疑
    SECURITY_CHECK_BLOCKED = -3      // 被阻止
} security_result_t;

// 攻击类型定义
typedef enum {
    ATTACK_TYPE_NONE = 0,
    ATTACK_TYPE_PATH_TRAVERSAL,     // 路径遍历
    ATTACK_TYPE_BUFFER_OVERFLOW,    // 缓冲区溢出
    ATTACK_TYPE_INJECTION,          // 注入攻击
    ATTACK_TYPE_DOS,                // 拒绝服务
    ATTACK_TYPE_BRUTE_FORCE,        // 暴力破解
    ATTACK_TYPE_MALFORMED_REQUEST   // 恶意请求
} attack_type_t;

// 安全配置
typedef struct {
    // 路径安全
    int enable_path_validation;     // 启用路径验证
    int max_path_depth;             // 最大路径深度
    char *forbidden_patterns[32];   // 禁止的路径模式
    int pattern_count;              // 模式数量
    
    // 请求安全
    size_t max_header_size;         // 最大头部大小
    size_t max_uri_length;          // 最大URI长度
    int max_headers_count;          // 最大头部数量
    
    // 速率限制
    int enable_rate_limiting;       // 启用速率限制
    int requests_per_second;        // 每秒请求数限制
    int burst_size;                 // 突发大小
    int ban_duration;               // 封禁时长(秒)
    
    // 内容过滤
    int enable_content_filtering;   // 启用内容过滤
    char *blocked_extensions[16];   // 阻止的文件扩展名
    int extension_count;            // 扩展名数量
    
    // 认证安全
    int max_auth_attempts;          // 最大认证尝试次数
    int auth_lockout_time;          // 认证锁定时间
    
    // 日志记录
    int log_security_events;        // 记录安全事件
    int log_blocked_requests;       // 记录被阻止的请求
} security_config_t;

// 客户端信息
typedef struct {
    char ip_address[46];            // IP地址(支持IPv6)
    time_t first_seen;              // 首次访问时间
    time_t last_seen;               // 最后访问时间
    uint32_t request_count;         // 请求计数
    uint32_t blocked_count;         // 被阻止次数
    uint32_t auth_failures;         // 认证失败次数
    time_t last_auth_failure;       // 最后认证失败时间
    int is_banned;                  // 是否被封禁
    time_t ban_until;               // 封禁到期时间
} client_info_t;

// 安全统计
typedef struct {
    uint64_t total_requests;        // 总请求数
    uint64_t blocked_requests;      // 被阻止的请求数
    uint64_t suspicious_requests;   // 可疑请求数
    uint64_t path_traversal_attempts; // 路径遍历尝试
    uint64_t injection_attempts;    // 注入攻击尝试
    uint64_t dos_attempts;          // DoS攻击尝试
    uint64_t brute_force_attempts;  // 暴力破解尝试
    uint64_t banned_clients;        // 被封禁的客户端数
    time_t last_attack_time;        // 最后攻击时间
    attack_type_t last_attack_type; // 最后攻击类型
} security_stats_t;

// 安全事件回调函数类型
typedef void (*security_event_callback_t)(attack_type_t type, const char *client_ip, 
                                         const char *details, time_t timestamp);

// 函数声明

/**
 * 初始化安全模块
 */
int security_init(const security_config_t *config);

/**
 * 销毁安全模块
 */
void security_destroy(void);

/**
 * 验证路径安全性
 */
security_result_t security_validate_path(const char *path);

/**
 * 验证URI安全性
 */
security_result_t security_validate_uri(const char *uri);

/**
 * 验证HTTP头部安全性
 */
security_result_t security_validate_headers(const char *headers, size_t headers_size);

/**
 * 检查速率限制
 */
security_result_t security_check_rate_limit(const char *client_ip);

/**
 * 检查客户端是否被封禁
 */
int security_is_client_banned(const char *client_ip);

/**
 * 记录认证失败
 */
void security_record_auth_failure(const char *client_ip);

/**
 * 记录安全事件
 */
void security_log_event(attack_type_t type, const char *client_ip, const char *details);

/**
 * 获取安全统计信息
 */
void security_get_stats(security_stats_t *stats);

/**
 * 重置安全统计
 */
void security_reset_stats(void);

/**
 * 打印安全报告
 */
void security_print_report(void);

/**
 * 设置安全事件回调
 */
void security_set_event_callback(security_event_callback_t callback);

/**
 * 获取默认安全配置
 */
security_config_t security_get_default_config(void);

/**
 * 清理过期的客户端信息
 */
void security_cleanup_expired_clients(void);

/**
 * 手动封禁客户端
 */
int security_ban_client(const char *client_ip, int duration_seconds);

/**
 * 解除客户端封禁
 */
int security_unban_client(const char *client_ip);

// 安全检查宏
#define SECURITY_CHECK_PATH(path) \
    ({ \
        security_result_t result = security_validate_path(path); \
        if (result != SECURITY_CHECK_PASS) { \
            log_warn("路径安全检查失败: %s", path); \
        } \
        result; \
    })

#define SECURITY_CHECK_CLIENT(ip) \
    ({ \
        int banned = security_is_client_banned(ip); \
        if (banned) { \
            log_info("客户端已被封禁: %s", ip); \
        } \
        banned; \
    })

#define SECURITY_SAFE_COPY(dest, src, size) \
    do { \
        if (src && dest && size > 0) { \
            strncpy(dest, src, size - 1); \
            dest[size - 1] = '\0'; \
        } \
    } while(0)

// 常用的安全常量
#define MAX_PATH_LENGTH 4096
#define MAX_URI_LENGTH 8192
#define MAX_HEADER_SIZE 8192
#define MAX_HEADERS_COUNT 100
#define DEFAULT_RATE_LIMIT 100
#define DEFAULT_BURST_SIZE 10
#define DEFAULT_BAN_DURATION 3600
#define MAX_AUTH_ATTEMPTS 5
#define AUTH_LOCKOUT_TIME 900

#endif // SECURITY_ENHANCED_H
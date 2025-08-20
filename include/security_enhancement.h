/**
 * 安全增强模块头文件
 * 提供额外的安全防护功能
 */

#ifndef SECURITY_ENHANCEMENT_H
#define SECURITY_ENHANCEMENT_H

#include <time.h>
#include <stdatomic.h>

// 安全配置
typedef struct {
    int enable_path_traversal_protection;  // 路径遍历防护
    int enable_sql_injection_protection;   // SQL注入防护
    int enable_xss_protection;             // XSS防护
    int enable_csrf_protection;            // CSRF防护
    int enable_rate_limiting;              // 速率限制
    int enable_ip_whitelist;               // IP白名单
    int enable_ip_blacklist;               // IP黑名单
    int max_request_size;                  // 最大请求大小
    int max_header_size;                   // 最大请求头大小
    int enable_ssl_termination;            // SSL终止
    char *ssl_cert_file;                   // SSL证书文件
    char *ssl_key_file;                    // SSL私钥文件
} security_config_t;

// 安全统计
typedef struct {
    atomic_uint_fast64_t blocked_requests;     // 被阻止的请求数
    atomic_uint_fast64_t path_traversal_attempts; // 路径遍历尝试数
    atomic_uint_fast64_t sql_injection_attempts;  // SQL注入尝试数
    atomic_uint_fast64_t xss_attempts;           // XSS尝试数
    atomic_uint_fast64_t csrf_attempts;          // CSRF尝试数
    atomic_uint_fast64_t rate_limited_requests;  // 速率限制请求数
    time_t last_attack_time;                     // 最后攻击时间
} security_stats_t;

// 函数声明

/**
 * 初始化安全模块
 * 
 * @param config 安全配置
 * @return 成功返回0，失败返回-1
 */
int init_security_enhancement(const security_config_t *config);

/**
 * 检查请求安全性
 * 
 * @param request_path 请求路径
 * @param request_headers 请求头
 * @param client_ip 客户端IP
 * @return 安全返回0，不安全返回-1
 */
int check_request_security(const char *request_path, 
                          const char *request_headers, 
                          const char *client_ip);

/**
 * 路径遍历防护
 * 
 * @param path 请求路径
 * @return 安全返回0，不安全返回-1
 */
int check_path_traversal(const char *path);

/**
 * SQL注入防护
 * 
 * @param input 输入字符串
 * @return 安全返回0，不安全返回-1
 */
int check_sql_injection(const char *input);

/**
 * XSS防护
 * 
 * @param input 输入字符串
 * @return 安全返回0，不安全返回-1
 */
int check_xss_attack(const char *input);

/**
 * CSRF防护
 * 
 * @param token CSRF令牌
 * @param session_id 会话ID
 * @return 有效返回0，无效返回-1
 */
int check_csrf_token(const char *token, const char *session_id);

/**
 * 速率限制检查
 * 
 * @param client_ip 客户端IP
 * @param request_type 请求类型
 * @return 允许返回0，拒绝返回-1
 */
int check_rate_limit(const char *client_ip, const char *request_type);

/**
 * IP白名单检查
 * 
 * @param client_ip 客户端IP
 * @return 在白名单中返回0，不在返回-1
 */
int check_ip_whitelist(const char *client_ip);

/**
 * IP黑名单检查
 * 
 * @param client_ip 客户端IP
 * @return 在黑名单中返回-1，不在返回0
 */
int check_ip_blacklist(const char *client_ip);

/**
 * 获取安全统计信息
 * 
 * @param stats 统计信息结构体指针
 */
void get_security_stats(security_stats_t *stats);

/**
 * 清理安全模块
 */
void cleanup_security_enhancement(void);

#endif /* SECURITY_ENHANCEMENT_H */ 
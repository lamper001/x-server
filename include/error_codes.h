/**
 * 错误码定义
 */

#ifndef ERROR_CODES_H
#define ERROR_CODES_H

// 系统错误码
typedef enum {
    ERR_SUCCESS = 0,
    
    // 网络相关错误 (1000-1099)
    ERR_SOCKET_CREATE = 1001,
    ERR_SOCKET_BIND = 1002,
    ERR_SOCKET_LISTEN = 1003,
    ERR_SOCKET_ACCEPT = 1004,
    ERR_SOCKET_CONNECT = 1005,
    ERR_SOCKET_TIMEOUT = 1006,
    
    // HTTP相关错误 (1100-1199)
    ERR_HTTP_PARSE = 1101,
    ERR_HTTP_METHOD = 1102,
    ERR_HTTP_URI_TOO_LONG = 1103,
    ERR_HTTP_HEADER_TOO_LARGE = 1104,
    ERR_HTTP_BODY_TOO_LARGE = 1105,
    
    // 认证相关错误 (1200-1299)
    ERR_AUTH_MISSING_TOKEN = 1201,
    ERR_AUTH_INVALID_TOKEN = 1202,
    ERR_AUTH_EXPIRED_TOKEN = 1203,
    ERR_AUTH_OAUTH_FAILED = 1204,
    
    // 文件相关错误 (1300-1399)
    ERR_FILE_NOT_FOUND = 1301,
    ERR_FILE_ACCESS_DENIED = 1302,
    ERR_FILE_TOO_LARGE = 1303,
    ERR_FILE_READ_ERROR = 1304,
    
    // 配置相关错误 (1400-1499)
    ERR_CONFIG_PARSE = 1401,
    ERR_CONFIG_MISSING = 1402,
    ERR_CONFIG_INVALID = 1403,
    
    // 内存相关错误 (1500-1599)
    ERR_MEMORY_ALLOC = 1501,
    ERR_MEMORY_POOL_FULL = 1502,
    
    // 代理相关错误 (1600-1699)
    ERR_PROXY_CONNECT = 1601,
    ERR_PROXY_TIMEOUT = 1602,
    ERR_PROXY_RESPONSE = 1603,
    
} error_code_t;

/**
 * 获取错误描述
 * 
 * @param code 错误码
 * @return 错误描述字符串
 */
const char *get_error_description(error_code_t code);

/**
 * 记录错误信息
 * 
 * @param code 错误码
 * @param context 上下文信息
 * @param file 文件名
 * @param line 行号
 */
void log_error_with_context(error_code_t code, const char *context, 
                           const char *file, int line);

// 便捷宏
#define LOG_ERROR(code, context) \
    log_error_with_context(code, context, __FILE__, __LINE__)

#endif /* ERROR_CODES_H */
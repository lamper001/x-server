/**
 * HTTP请求处理模块
 */

#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>

// HTTP请求方法
typedef enum {
    HTTP_GET,
    HTTP_POST,
    HTTP_PUT,
    HTTP_DELETE,
    HTTP_HEAD,
    HTTP_OPTIONS,
    HTTP_UNKNOWN
} http_method_t;

// HTTP请求头
typedef struct {
    char *name;
    char *value;
} http_header_t;

// HTTP请求结构体
typedef struct {
    http_method_t method;
    char *path;
    char *query_string;
    char *version;
    http_header_t *headers;
    int header_count;
    char *body;
    size_t body_length;
} http_request_t;

/**
 * 解析HTTP请求
 * 
 * @param client_sock 客户端套接字
 * @param request 请求结构体指针
 * @return 成功返回0，失败返回非0值
 */
int parse_http_request(int client_sock, http_request_t *request);

/**
 * 解析HTTP请求（安全版本）
 * 
 * @param client_sock 客户端套接字
 * @param request 请求结构体指针
 * @return 成功返回0，失败返回-1
 */
int parse_http_request_safe(int client_sock, http_request_t *request);

/**
 * 从缓冲区解析HTTP请求（适用于非阻塞I/O）
 * 
 * @param buffer 缓冲区
 * @param buffer_len 缓冲区长度
 * @param request 请求结构体指针
 * @return 成功返回0，失败返回-1，需要更多数据返回-2
 */
int parse_http_request_from_buffer(const char *buffer, size_t buffer_len, http_request_t *request);

/**
 * 获取HTTP方法字符串
 * 
 * @param method HTTP方法枚举值
 * @return 方法字符串
 */
const char *http_method_str(http_method_t method);

/**
 * 发送HTTP错误响应
 * 
 * @param client_sock 客户端套接字
 * @param status_code 状态码
 * @param message 错误消息
 * @param charset 字符集编码
 */
void send_http_error(int client_sock, int status_code, const char *message, const char *charset);

/**
 * 获取请求头的值
 * 
 * @param request 请求结构体
 * @param name 请求头名称
 * @return 请求头的值，未找到返回NULL
 */
char *get_header_value(http_request_t *request, const char *name);

/**
 * 释放HTTP请求结构体
 * 
 * @param request 请求结构体指针
 */
void free_http_request(http_request_t *request);

#endif /* HTTP_H */
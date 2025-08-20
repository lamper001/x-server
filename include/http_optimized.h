/**
 * 优化的HTTP解析器 - 实现HTTP状态机和批量解析
 */

#ifndef HTTP_OPTIMIZED_H
#define HTTP_OPTIMIZED_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "http.h"

// HTTP解析状态
typedef enum {
    HTTP_PARSE_START,           // 开始解析
    HTTP_PARSE_METHOD,          // 解析HTTP方法
    HTTP_PARSE_URI,             // 解析URI
    HTTP_PARSE_VERSION,         // 解析HTTP版本
    HTTP_PARSE_HEADER_NAME,     // 解析头部名称
    HTTP_PARSE_HEADER_VALUE,    // 解析头部值
    HTTP_PARSE_HEADER_END,      // 头部结束
    HTTP_PARSE_BODY,            // 解析请求体
    HTTP_PARSE_COMPLETE,        // 解析完成
    HTTP_PARSE_ERROR            // 解析错误
} http_parse_state_t;

// HTTP解析器结构
typedef struct {
    http_parse_state_t state;   // 当前解析状态
    size_t pos;                 // 当前解析位置
    size_t line_start;          // 当前行开始位置
    size_t header_start;        // 当前头部开始位置
    
    // 临时缓冲区
    char method_buffer[32];     // 方法缓冲区
    char uri_buffer[2048];      // URI缓冲区
    char version_buffer[16];    // 版本缓冲区
    char header_name_buffer[256]; // 头部名称缓冲区
    char header_value_buffer[4096]; // 头部值缓冲区
    
    // 解析结果
    http_request_t *request;    // HTTP请求结构
    bool has_content_length;    // 是否有Content-Length
    size_t content_length;      // Content-Length值
    bool chunked_transfer;      // 是否分块传输
    
    // 性能统计
    uint64_t parse_time_ns;     // 解析耗时（纳秒）
    uint64_t bytes_processed;   // 处理的字节数
    uint32_t parse_count;       // 解析次数
} http_parser_t;

// 批量解析结果
typedef struct {
    http_request_t **requests;  // 请求数组
    size_t count;               // 请求数量
    size_t capacity;            // 容量
    size_t total_bytes;         // 总字节数
    uint64_t parse_time_ns;     // 总解析时间
} http_batch_result_t;

// 函数声明

/**
 * 创建HTTP解析器
 */
http_parser_t *http_parser_create(void);

/**
 * 销毁HTTP解析器
 */
void http_parser_destroy(http_parser_t *parser);

/**
 * 重置HTTP解析器状态
 */
void http_parser_reset(http_parser_t *parser);

/**
 * 解析HTTP请求（状态机版本）
 * @param parser HTTP解析器
 * @param buffer 输入缓冲区
 * @param buffer_len 缓冲区长度
 * @return 解析的字节数，-1表示错误，0表示需要更多数据
 */
ssize_t http_parser_parse(http_parser_t *parser, const char *buffer, size_t buffer_len);

/**
 * 批量解析HTTP请求
 * @param buffer 输入缓冲区
 * @param buffer_len 缓冲区长度
 * @param max_requests 最大请求数
 * @return 批量解析结果
 */
http_batch_result_t *http_parser_parse_batch(const char *buffer, size_t buffer_len, size_t max_requests);

/**
 * 释放批量解析结果
 */
void http_batch_result_destroy(http_batch_result_t *result);

/**
 * 获取解析器统计信息
 */
void http_parser_get_stats(http_parser_t *parser, uint64_t *parse_time_ns, uint64_t *bytes_processed, uint32_t *parse_count);

/**
 * 重置解析器统计信息
 */
void http_parser_reset_stats(http_parser_t *parser);

/**
 * 优化的HTTP请求解析（兼容原有接口）
 */
int parse_http_request_optimized(int client_sock, http_request_t *request);

/**
 * 从缓冲区解析HTTP请求（优化版本）
 */
int parse_http_request_from_buffer_optimized(const char *buffer, size_t buffer_len, http_request_t *request);

#endif // HTTP_OPTIMIZED_H 
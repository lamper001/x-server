/**
 * 优化的HTTP解析器实现
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>
#include "../include/http_optimized.h"
#include "../include/logger.h"

// 常量定义
#define MAX_HEADERS 100
#define MAX_LINE_LENGTH 8192
#define MAX_URI_LENGTH 2048

// 前向声明
http_method_t parse_method(const char *method_str);
void free_http_request(http_request_t *request);

// 内联函数：获取高精度时间（纳秒）
static inline uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

// 内联函数：安全的字符检查
static inline bool is_http_token_char(char c) {
    return c > 0x1F && c != 0x7F && c != ' ' && c != '\t' && c != '\r' && c != '\n';
}

// 内联函数：安全的头部值字符检查
static inline bool is_http_header_value_char(char c) {
    return c >= 0x20 && c != 0x7F;
}

// 创建HTTP解析器
http_parser_t *http_parser_create(void) {
    http_parser_t *parser = malloc(sizeof(http_parser_t));
    if (!parser) {
        log_error("无法分配HTTP解析器内存");
        return NULL;
    }
    
    // 初始化解析器
    memset(parser, 0, sizeof(http_parser_t));
    parser->state = HTTP_PARSE_START;
    parser->request = malloc(sizeof(http_request_t));
    
    if (!parser->request) {
        log_error("无法分配HTTP请求内存");
        free(parser);
        return NULL;
    }
    
    memset(parser->request, 0, sizeof(http_request_t));
    parser->request->headers = malloc(sizeof(http_header_t) * MAX_HEADERS);
    
    if (!parser->request->headers) {
        log_error("无法分配HTTP头部内存");
        free(parser->request);
        free(parser);
        return NULL;
    }
    
    return parser;
}

// 销毁HTTP解析器
void http_parser_destroy(http_parser_t *parser) {
    if (!parser) return;
    
    if (parser->request) {
        free_http_request(parser->request);
        free(parser->request);
    }
    
    free(parser);
}

// 重置HTTP解析器状态
void http_parser_reset(http_parser_t *parser) {
    if (!parser) return;
    
    parser->state = HTTP_PARSE_START;
    parser->pos = 0;
    parser->line_start = 0;
    parser->header_start = 0;
    parser->has_content_length = false;
    parser->content_length = 0;
    parser->chunked_transfer = false;
    
    // 清空缓冲区
    memset(parser->method_buffer, 0, sizeof(parser->method_buffer));
    memset(parser->uri_buffer, 0, sizeof(parser->uri_buffer));
    memset(parser->version_buffer, 0, sizeof(parser->version_buffer));
    memset(parser->header_name_buffer, 0, sizeof(parser->header_name_buffer));
    memset(parser->header_value_buffer, 0, sizeof(parser->header_value_buffer));
    
    // 重置请求结构
    if (parser->request) {
        free_http_request(parser->request);
        memset(parser->request, 0, sizeof(http_request_t));
        parser->request->headers = malloc(sizeof(http_header_t) * MAX_HEADERS);
        if (!parser->request->headers) {
            log_error("重置时无法分配HTTP头部内存");
        }
    }
}

// 解析HTTP请求（状态机版本）
ssize_t http_parser_parse(http_parser_t *parser, const char *buffer, size_t buffer_len) {
    if (!parser || !buffer) return -1;
    
    uint64_t start_time = get_time_ns();
    size_t initial_pos = parser->pos;
    
    while (parser->pos < buffer_len && parser->state != HTTP_PARSE_COMPLETE && parser->state != HTTP_PARSE_ERROR) {
        char c = buffer[parser->pos];
        
        switch (parser->state) {
            case HTTP_PARSE_START:
                // 跳过前导空白字符
                if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                    parser->pos++;
                    continue;
                }
                parser->state = HTTP_PARSE_METHOD;
                parser->line_start = parser->pos;
                // 继续解析方法
                
            case HTTP_PARSE_METHOD:
                if (c == ' ') {
                    // 方法解析完成
                    size_t method_len = parser->pos - parser->line_start;
                    if (method_len >= sizeof(parser->method_buffer)) {
                        parser->state = HTTP_PARSE_ERROR;
                        break;
                    }
                    strncpy(parser->method_buffer, buffer + parser->line_start, method_len);
                    parser->method_buffer[method_len] = '\0';
                    parser->state = HTTP_PARSE_URI;
                    parser->pos++;
                    parser->line_start = parser->pos;
                } else if (!is_http_token_char(c)) {
                    parser->state = HTTP_PARSE_ERROR;
                } else {
                    parser->pos++;
                }
                break;
                
            case HTTP_PARSE_URI:
                if (c == ' ') {
                    // URI解析完成
                    size_t uri_len = parser->pos - parser->line_start;
                    if (uri_len >= sizeof(parser->uri_buffer)) {
                        parser->state = HTTP_PARSE_ERROR;
                        break;
                    }
                    strncpy(parser->uri_buffer, buffer + parser->line_start, uri_len);
                    parser->uri_buffer[uri_len] = '\0';
                    parser->state = HTTP_PARSE_VERSION;
                    parser->pos++;
                    parser->line_start = parser->pos;
                } else if (c < 0x20 || c == 0x7F) {
                    parser->state = HTTP_PARSE_ERROR;
                } else {
                    parser->pos++;
                }
                break;
                
            case HTTP_PARSE_VERSION:
                if (c == '\r' || c == '\n') {
                    // 版本解析完成
                    size_t version_len = parser->pos - parser->line_start;
                    if (version_len >= sizeof(parser->version_buffer)) {
                        parser->state = HTTP_PARSE_ERROR;
                        break;
                    }
                    strncpy(parser->version_buffer, buffer + parser->line_start, version_len);
                    parser->version_buffer[version_len] = '\0';
                    
                    // 验证版本格式
                    if (strncmp(parser->version_buffer, "HTTP/", 5) != 0) {
                        parser->state = HTTP_PARSE_ERROR;
                        break;
                    }
                    
                    // 设置请求方法
                    parser->request->method = parse_method(parser->method_buffer);
                    if (parser->request->method == HTTP_UNKNOWN) {
                        parser->state = HTTP_PARSE_ERROR;
                        break;
                    }
                    
                    // 设置路径
                    parser->request->path = strdup(parser->uri_buffer);
                    if (!parser->request->path) {
                        parser->state = HTTP_PARSE_ERROR;
                        break;
                    }
                    
                    // 解析查询字符串
                    char *query_string = strchr(parser->request->path, '?');
                    if (query_string) {
                        *query_string = '\0';
                        parser->request->query_string = strdup(query_string + 1);
                    }
                    
                    parser->state = HTTP_PARSE_HEADER_NAME;
                    parser->pos++;
                    parser->line_start = parser->pos;
                } else if (!is_http_token_char(c) && c != '.' && c != '/') {
                    parser->state = HTTP_PARSE_ERROR;
                } else {
                    parser->pos++;
                }
                break;
                
            case HTTP_PARSE_HEADER_NAME:
                if (c == '\r' || c == '\n') {
                    // 空行，头部解析完成
                    if (c == '\r' && parser->pos + 1 < buffer_len && buffer[parser->pos + 1] == '\n') {
                        parser->pos += 2;
                    } else {
                        parser->pos++;
                    }
                    parser->state = HTTP_PARSE_HEADER_END;
                } else if (c == ':') {
                    // 头部名称解析完成
                    size_t name_len = parser->pos - parser->line_start;
                    if (name_len >= sizeof(parser->header_name_buffer)) {
                        parser->state = HTTP_PARSE_ERROR;
                        break;
                    }
                    strncpy(parser->header_name_buffer, buffer + parser->line_start, name_len);
                    parser->header_name_buffer[name_len] = '\0';
                    parser->state = HTTP_PARSE_HEADER_VALUE;
                    parser->pos++;
                    parser->line_start = parser->pos;
                } else if (!is_http_token_char(c)) {
                    parser->state = HTTP_PARSE_ERROR;
                } else {
                    parser->pos++;
                }
                break;
                
            case HTTP_PARSE_HEADER_VALUE:
                if (c == '\r' || c == '\n') {
                    // 头部值解析完成
                    size_t value_len = parser->pos - parser->line_start;
                    if (value_len >= sizeof(parser->header_value_buffer)) {
                        parser->state = HTTP_PARSE_ERROR;
                        break;
                    }
                    strncpy(parser->header_value_buffer, buffer + parser->line_start, value_len);
                    parser->header_value_buffer[value_len] = '\0';
                    
                    // 添加到请求头部
                    if (parser->request->header_count < MAX_HEADERS) {
                        http_header_t *header = &parser->request->headers[parser->request->header_count];
                        header->name = strdup(parser->header_name_buffer);
                        header->value = strdup(parser->header_value_buffer);
                        parser->request->header_count++;
                        
                        // 检查特殊头部
                        if (strcasecmp(parser->header_name_buffer, "Content-Length") == 0) {
                            parser->has_content_length = true;
                            parser->content_length = strtoul(parser->header_value_buffer, NULL, 10);
                        } else if (strcasecmp(parser->header_name_buffer, "Transfer-Encoding") == 0) {
                            if (strcasecmp(parser->header_value_buffer, "chunked") == 0) {
                                parser->chunked_transfer = true;
                            }
                        }
                    }
                    
                    if (c == '\r' && parser->pos + 1 < buffer_len && buffer[parser->pos + 1] == '\n') {
                        parser->pos += 2;
                    } else {
                        parser->pos++;
                    }
                    parser->state = HTTP_PARSE_HEADER_NAME;
                    parser->line_start = parser->pos;
                } else if (!is_http_header_value_char(c)) {
                    parser->state = HTTP_PARSE_ERROR;
                } else {
                    parser->pos++;
                }
                break;
                
            case HTTP_PARSE_HEADER_END:
                // 检查是否需要解析请求体
                if (parser->has_content_length && parser->content_length > 0) {
                    parser->state = HTTP_PARSE_BODY;
                } else if (parser->chunked_transfer) {
                    parser->state = HTTP_PARSE_BODY;
                } else {
                    parser->state = HTTP_PARSE_COMPLETE;
                }
                break;
                
            case HTTP_PARSE_BODY:
                // 简化处理：如果有Content-Length，检查是否足够
                if (parser->has_content_length) {
                    size_t remaining = buffer_len - parser->pos;
                    if (remaining >= parser->content_length) {
                        // 请求体完整
                        parser->request->body = malloc(parser->content_length + 1);
                        if (parser->request->body) {
                            memcpy(parser->request->body, buffer + parser->pos, parser->content_length);
                            parser->request->body[parser->content_length] = '\0';
                            parser->request->body_length = parser->content_length;
                            parser->pos += parser->content_length;
                        }
                        parser->state = HTTP_PARSE_COMPLETE;
                    } else {
                        // 需要更多数据
                        return 0;
                    }
                } else {
                    // 没有Content-Length，认为解析完成
                    parser->state = HTTP_PARSE_COMPLETE;
                }
                break;
                
            default:
                parser->state = HTTP_PARSE_ERROR;
                break;
        }
    }
    
    // 更新统计信息
    parser->bytes_processed += parser->pos - initial_pos;
    if (parser->state == HTTP_PARSE_COMPLETE) {
        parser->parse_count++;
        parser->parse_time_ns += get_time_ns() - start_time;
    }
    
    if (parser->state == HTTP_PARSE_ERROR) {
        return -1;
    } else if (parser->state == HTTP_PARSE_COMPLETE) {
        return parser->pos;
    } else {
        return 0; // 需要更多数据
    }
}

// 批量解析HTTP请求
http_batch_result_t *http_parser_parse_batch(const char *buffer, size_t buffer_len, size_t max_requests) {
    http_batch_result_t *result = malloc(sizeof(http_batch_result_t));
    if (!result) {
        log_error("无法分配批量解析结果内存");
        return NULL;
    }
    
    result->requests = malloc(sizeof(http_request_t *) * max_requests);
    if (!result->requests) {
        log_error("无法分配请求数组内存");
        free(result);
        return NULL;
    }
    
    result->count = 0;
    result->capacity = max_requests;
    result->total_bytes = 0;
    result->parse_time_ns = get_time_ns();
    
    http_parser_t *parser = http_parser_create();
    if (!parser) {
        free(result->requests);
        free(result);
        return NULL;
    }
    
    size_t pos = 0;
    while (pos < buffer_len && result->count < max_requests) {
        http_parser_reset(parser);
        
        ssize_t parsed = http_parser_parse(parser, buffer + pos, buffer_len - pos);
        if (parsed > 0) {
            // 解析成功
            result->requests[result->count] = malloc(sizeof(http_request_t));
            if (result->requests[result->count]) {
                memcpy(result->requests[result->count], parser->request, sizeof(http_request_t));
                // 深拷贝头部
                if (parser->request->headers && parser->request->header_count > 0) {
                    result->requests[result->count]->headers = malloc(sizeof(http_header_t) * parser->request->header_count);
                    if (result->requests[result->count]->headers) {
                        for (int i = 0; i < parser->request->header_count; i++) {
                            result->requests[result->count]->headers[i].name = strdup(parser->request->headers[i].name);
                            result->requests[result->count]->headers[i].value = strdup(parser->request->headers[i].value);
                        }
                        result->requests[result->count]->header_count = parser->request->header_count;
                    }
                }
                // 深拷贝其他字段
                if (parser->request->path) {
                    result->requests[result->count]->path = strdup(parser->request->path);
                }
                if (parser->request->query_string) {
                    result->requests[result->count]->query_string = strdup(parser->request->query_string);
                }
                if (parser->request->body) {
                    result->requests[result->count]->body = malloc(parser->request->body_length + 1);
                    if (result->requests[result->count]->body) {
                        memcpy(result->requests[result->count]->body, parser->request->body, parser->request->body_length);
                        result->requests[result->count]->body[parser->request->body_length] = '\0';
                        result->requests[result->count]->body_length = parser->request->body_length;
                    }
                }
            }
            result->count++;
            pos += parsed;
            result->total_bytes += parsed;
        } else if (parsed == 0) {
            // 需要更多数据
            break;
        } else {
            // 解析错误，跳过当前字符
            pos++;
        }
    }
    
    result->parse_time_ns = get_time_ns() - result->parse_time_ns;
    http_parser_destroy(parser);
    
    return result;
}

// 释放批量解析结果
void http_batch_result_destroy(http_batch_result_t *result) {
    if (!result) return;
    
    for (size_t i = 0; i < result->count; i++) {
        if (result->requests[i]) {
            free_http_request(result->requests[i]);
            free(result->requests[i]);
        }
    }
    
    free(result->requests);
    free(result);
}

// 获取解析器统计信息
void http_parser_get_stats(http_parser_t *parser, uint64_t *parse_time_ns, uint64_t *bytes_processed, uint32_t *parse_count) {
    if (!parser) return;
    
    if (parse_time_ns) *parse_time_ns = parser->parse_time_ns;
    if (bytes_processed) *bytes_processed = parser->bytes_processed;
    if (parse_count) *parse_count = parser->parse_count;
}

// 重置解析器统计信息
void http_parser_reset_stats(http_parser_t *parser) {
    if (!parser) return;
    
    parser->parse_time_ns = 0;
    parser->bytes_processed = 0;
    parser->parse_count = 0;
}

// 优化的HTTP请求解析（兼容原有接口）
int parse_http_request_optimized(int client_sock, http_request_t *request) {
    char buffer[8192];
    ssize_t bytes_read = recv(client_sock, buffer, sizeof(buffer) - 1, 0);
    
    if (bytes_read <= 0) {
        return -1;
    }
    
    buffer[bytes_read] = '\0';
    return parse_http_request_from_buffer_optimized(buffer, bytes_read, request);
}

// 从缓冲区解析HTTP请求（优化版本）
int parse_http_request_from_buffer_optimized(const char *buffer, size_t buffer_len, http_request_t *request) {
    http_parser_t *parser = http_parser_create();
    if (!parser) {
        return -1;
    }
    
    ssize_t result = http_parser_parse(parser, buffer, buffer_len);
    
    if (result > 0) {
        // 复制解析结果
        memcpy(request, parser->request, sizeof(http_request_t));
        
        // 深拷贝动态分配的内容
        if (parser->request->path) {
            request->path = strdup(parser->request->path);
        }
        if (parser->request->query_string) {
            request->query_string = strdup(parser->request->query_string);
        }
        if (parser->request->body) {
            request->body = malloc(parser->request->body_length + 1);
            if (request->body) {
                memcpy(request->body, parser->request->body, parser->request->body_length);
                request->body[parser->request->body_length] = '\0';
            }
        }
        if (parser->request->headers && parser->request->header_count > 0) {
            request->headers = malloc(sizeof(http_header_t) * parser->request->header_count);
            if (request->headers) {
                for (int i = 0; i < parser->request->header_count; i++) {
                    request->headers[i].name = strdup(parser->request->headers[i].name);
                    request->headers[i].value = strdup(parser->request->headers[i].value);
                }
                request->header_count = parser->request->header_count;
            }
        }
        
        http_parser_destroy(parser);
        return 0;
    }
    
    http_parser_destroy(parser);
    return -1;
} 
/**
 * proxy转发模块实现
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>

#include "../include/proxy.h"
#include "../include/http.h"
#include "../include/config.h"
#include "../include/logger.h"

#define BUFFER_SIZE 8192
#define TIMEOUT_MS 30000  // 30秒超时

// Upstream server error types
typedef enum {
    UPSTREAM_ERROR_NONE = 0,
    UPSTREAM_ERROR_CONNECT_FAILED,
    UPSTREAM_ERROR_TIMEOUT,
    UPSTREAM_ERROR_DNS_FAILED,
    UPSTREAM_ERROR_READ_FAILED,
    UPSTREAM_ERROR_WRITE_FAILED
} upstream_error_t;

// Create connection to target server (with timeout and retry)
static int connect_to_server(const char *host, int port, upstream_error_t *error) {
    struct addrinfo hints, *res, *res_start;
    int server_sock;
    char port_str[10];
    
    if (error) *error = UPSTREAM_ERROR_NONE;
    
    snprintf(port_str, sizeof(port_str), "%d", port);
    
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    
    // DNS解析
    int dns_result = getaddrinfo(host, port_str, &hints, &res);
    if (dns_result != 0) {
        log_error("DNS resolution failed: %s:%d - %s", host, port, gai_strerror(dns_result));
        if (error) *error = UPSTREAM_ERROR_DNS_FAILED;
        return -1;
    }
    
    res_start = res;
    server_sock = -1;
    
    // Try to connect to resolved address
    while (res != NULL) {
        server_sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (server_sock < 0) {
            res = res->ai_next;
            continue;
        }
        
        // Set connection timeout
        struct timeval timeout;
        timeout.tv_sec = 5;  // 5 second connection timeout
        timeout.tv_usec = 0;
        setsockopt(server_sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(server_sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        
        if (connect(server_sock, res->ai_addr, res->ai_addrlen) == 0) {
            break;  // Connection successful
        }
        
        log_debug("Connection failed: %s:%d - %s", host, port, strerror(errno));
        close(server_sock);
        server_sock = -1;
        res = res->ai_next;
    }
    
    freeaddrinfo(res_start);
    
    if (server_sock < 0) {
        log_error("Unable to connect to upstream server %s:%d", host, port);
        if (error) *error = UPSTREAM_ERROR_CONNECT_FAILED;
        return -1;
    }
    
    return server_sock;
}

// Send nginx-style error page
static void send_upstream_error_page(int client_sock, int status_code, const char *error_msg, const char *upstream_info) {
    char html_body[1024];
    char response_headers[512];
    
    // Build HTML error page
    int html_len = snprintf(html_body, sizeof(html_body),
        "<!DOCTYPE html>\n"
        "<html>\n"
        "<head>\n"
        "    <title>%d %s</title>\n"
        "    <style>\n"
        "        body { font-family: Arial, sans-serif; margin: 40px; }\n"
        "        .error-container { max-width: 600px; margin: 0 auto; }\n"
        "        .error-code { font-size: 72px; font-weight: bold; color: #dc3545; margin-bottom: 20px; }\n"
        "        .error-message { font-size: 24px; margin-bottom: 20px; }\n"
        "        .error-details { color: #666; font-size: 14px; }\n"
        "        .upstream-info { background: #f8f9fa; padding: 10px; border-left: 4px solid #dc3545; margin-top: 20px; }\n"
        "    </style>\n"
        "</head>\n"
        "<body>\n"
        "    <div class=\"error-container\">\n"
        "        <div class=\"error-code\">%d</div>\n"
        "        <div class=\"error-message\">%s</div>\n"
        "        <div class=\"error-details\">The server encountered an error while trying to fulfill your request.</div>\n"
        "        <div class=\"upstream-info\"><strong>Upstream:</strong> %s</div>\n"
        "    </div>\n"
        "</body>\n"
        "</html>",
        status_code, error_msg, status_code, error_msg, upstream_info ? upstream_info : "unknown");
    
    // 构建HTTP响应头
    int header_len = snprintf(response_headers, sizeof(response_headers),
        "HTTP/1.1 %d %s\r\n"
        "Server: X-Server\r\n"
        "Content-Type: text/html; charset=UTF-8\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "Cache-Control: no-cache\r\n"
        "\r\n",
        status_code, error_msg, html_len);
    
    // 分别发送响应头和响应体，确保完整性
    ssize_t bytes_written = 0;
    ssize_t total_to_write = header_len;
    
    // 发送响应头
    while (bytes_written < total_to_write) {
        ssize_t n = write(client_sock, response_headers + bytes_written, total_to_write - bytes_written);
        if (n <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(1000);
                continue;
            }
            log_error("Failed to send error page response headers: %s", strerror(errno));
            return;
        }
        bytes_written += n;
    }
    
    // 发送响应体
    bytes_written = 0;
    total_to_write = html_len;
    while (bytes_written < total_to_write) {
        ssize_t n = write(client_sock, html_body + bytes_written, total_to_write - bytes_written);
        if (n <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(1000);
                continue;
            }
            log_error("Failed to send error page response body: %s", strerror(errno));
            return;
        }
        bytes_written += n;
    }
}

// 重写请求路径
static char *rewrite_path(const char *original_path, const char *prefix) {
    if (original_path == NULL || prefix == NULL) {
        return NULL;
    }
    
    size_t prefix_len = strlen(prefix);
    size_t path_len = strlen(original_path);
    
    // 如果路径不以前缀开头，返回原始路径
    if (strncmp(original_path, prefix, prefix_len) != 0) {
        return strdup(original_path);
    }
    
    // 移除前缀，保留路径的其余部分
    if (path_len <= prefix_len) {
        return strdup("/");
    } else {
        return strdup(original_path + prefix_len);
    }
}

// 解析HTTP响应状态码
static int parse_response_status_code(const char *response) {
    // 检查响应是否以"HTTP/"开头
    if (strncmp(response, "HTTP/", 5) != 0) {
        return 200;  // 默认状态码
    }
    
    // 查找第一个空格
    const char *space = strchr(response, ' ');
    if (space == NULL) {
        return 200;
    }
    
    // 解析状态码
    return atoi(space + 1);
}

// 解析Content-Length头
static long parse_content_length(const char *response) {
    const char *content_length_header = strstr(response, "Content-Length:");
    if (content_length_header == NULL) {
        content_length_header = strstr(response, "content-length:");
    }
    
    if (content_length_header == NULL) {
        return 0;  // 没有Content-Length头
    }
    
    // 跳过"Content-Length:"
    content_length_header += 15;
    
    // 跳过空格
    while (*content_length_header == ' ') {
        content_length_header++;
    }
    
    // 解析数字
    return atol(content_length_header);
}

// Forward request to target server
int proxy_request(int client_sock, http_request_t *request, route_t *route, int *status_code, size_t *response_size) {
    upstream_error_t upstream_error;
    char upstream_info[256];
    
    snprintf(upstream_info, sizeof(upstream_info), "%s:%d", route->target_host, route->target_port);
    
    int server_sock = connect_to_server(route->target_host, route->target_port, &upstream_error);
    if (server_sock < 0) {
        // Return different status codes and error messages based on different error types
        int error_status;
        const char *error_msg;
        
        switch (upstream_error) {
            case UPSTREAM_ERROR_DNS_FAILED:
                error_status = 502;
                error_msg = "Bad Gateway - DNS Resolution Failed";
                break;
            case UPSTREAM_ERROR_CONNECT_FAILED:
                error_status = 502;
                error_msg = "Bad Gateway - Connection Failed";
                break;
            case UPSTREAM_ERROR_TIMEOUT:
                error_status = 504;
                error_msg = "Gateway Timeout";
                break;
            default:
                error_status = 502;
                error_msg = "Bad Gateway";
                break;
        }
        
        log_warn("Proxy request failed: %s - %s", upstream_info, error_msg);
        send_upstream_error_page(client_sock, error_status, error_msg, upstream_info);
        if (status_code) *status_code = error_status;
        if (response_size) *response_size = 1024; // Estimate error page size
        return -1;
    }
    
    // 构建转发请求
    char buffer[BUFFER_SIZE];
    char *method_str;
    
    switch (request->method) {
        case HTTP_GET: method_str = "GET"; break;
        case HTTP_POST: method_str = "POST"; break;
        case HTTP_PUT: method_str = "PUT"; break;
        case HTTP_DELETE: method_str = "DELETE"; break;
        case HTTP_HEAD: method_str = "HEAD"; break;
        case HTTP_OPTIONS: method_str = "OPTIONS"; break;
        default: method_str = "GET"; break;
    }
    
    // 重写请求路径
    char *new_path = rewrite_path(request->path, route->path_prefix);
    if (new_path == NULL) {
        log_error("Path rewrite failed");
        close(server_sock);
        if (status_code) *status_code = 500;
        if (response_size) *response_size = 0;
        return -1;
    }
    
    // 构建请求行 - 使用更安全的方式
    size_t remaining = sizeof(buffer);
    int len = snprintf(buffer, remaining, "%s %s", method_str, new_path);
    if (len < 0 || len >= (int)remaining) {
        log_error("Failed to build request line, path too long: %s", new_path);
        close(server_sock);
        free(new_path);
        if (status_code) *status_code = 414;  // URI Too Long
        if (response_size) *response_size = 0;
        return -1;
    }
    remaining -= len;
    
    // 添加查询字符串（如果有）
    if (request->query_string != NULL) {
        int ret = snprintf(buffer + len, sizeof(buffer) - len, "?%s", request->query_string);
        if (ret < 0 || ret >= (int)(sizeof(buffer) - len)) {
            log_error("Failed to add query string, insufficient buffer");
            close(server_sock);
            free(new_path);
            if (status_code) *status_code = 500;
            if (response_size) *response_size = 0;
            return -1;
        }
        len += ret;
    }
    
    // 添加HTTP版本
    int ret = snprintf(buffer + len, sizeof(buffer) - len, " %s\r\n", request->version);
    if (ret < 0 || ret >= (int)(sizeof(buffer) - len)) {
        log_error("Failed to add HTTP version, insufficient buffer");
        close(server_sock);
        free(new_path);
        if (status_code) *status_code = 500;
        if (response_size) *response_size = 0;
        return -1;
    }
    len += ret;
    
    // 安全的请求头添加 - 防止CRLF注入
    for (int i = 0; i < request->header_count; i++) {
        // 跳过Connection头，我们将使用自己的
        if (strcasecmp(request->headers[i].name, "Connection") == 0) {
            continue;
        }
        
        // 跳过可能危险的头部
        if (strcasecmp(request->headers[i].name, "Transfer-Encoding") == 0 ||
            strcasecmp(request->headers[i].name, "Content-Encoding") == 0 ||
            strcasecmp(request->headers[i].name, "Upgrade") == 0) {
            log_warn("跳过潜在危险的头部: %s", request->headers[i].name);
            continue;
        }
        
        // 验证头部名称和值的安全性
        const char *name = request->headers[i].name;
        const char *value = request->headers[i].value;
        
        if (!name || !value) continue;
        
        // 检查头部名称中的危险字符
        int name_safe = 1;
        for (size_t j = 0; name[j]; j++) {
            char c = name[j];
            if (c < 0x21 || c > 0x7E || c == ':' || c == ' ' || c == '\t' || 
                c == '\r' || c == '\n') {
                name_safe = 0;
                break;
            }
        }
        
        if (!name_safe) {
            log_warn("头部名称包含危险字符，跳过: %s", name);
            continue;
        }
        
        // 检查头部值中的CRLF注入
        int value_safe = 1;
        for (size_t j = 0; value[j]; j++) {
            char c = value[j];
            if (c == '\r' || c == '\n') {
                value_safe = 0;
                break;
            }
            // 检查其他控制字符（除了制表符）
            if (c < 0x20 && c != '\t') {
                value_safe = 0;
                break;
            }
        }
        
        if (!value_safe) {
            log_warn("头部值包含危险字符，跳过: %s: %s", name, value);
            continue;
        }
        
        // 检查缓冲区空间
        if (len >= (int)sizeof(buffer) - 100) { // 保留100字节用于后续头部
            log_warn("请求头过多，可能被截断");
            break;
        }
        
        // 安全地添加头部
        ret = snprintf(buffer + len, sizeof(buffer) - len, "%s: %s\r\n", name, value);
        if (ret < 0 || ret >= (int)(sizeof(buffer) - len)) {
            log_warn("添加请求头失败，缓冲区不足: %s", name);
            break;
        }
        len += ret;
    }
    
    // 添加X-Forwarded-For头
    if (len < (int)sizeof(buffer) - 50) { // 确保有足够空间
        char *client_ip = get_header_value(request, "X-Forwarded-For");
        if (client_ip != NULL) {
            ret = snprintf(buffer + len, sizeof(buffer) - len, "X-Forwarded-For: %s\r\n", client_ip);
        } else {
            ret = snprintf(buffer + len, sizeof(buffer) - len, "X-Forwarded-For: %s\r\n", "unknown");
        }
        if (ret > 0 && ret < (int)(sizeof(buffer) - len)) {
            len += ret;
        }
    }
    
    // 添加X-Forwarded-Host头
    if (len < (int)sizeof(buffer) - 50) { // 确保有足够空间
        char *host = get_header_value(request, "Host");
        if (host != NULL) {
            ret = snprintf(buffer + len, sizeof(buffer) - len, "X-Forwarded-Host: %s\r\n", host);
            if (ret > 0 && ret < (int)(sizeof(buffer) - len)) {
                len += ret;
            }
        }
    }
    
    // 添加Connection: close头
    if (len < (int)sizeof(buffer) - 30) {
        ret = snprintf(buffer + len, sizeof(buffer) - len, "Connection: close\r\n");
        if (ret > 0 && ret < (int)(sizeof(buffer) - len)) {
            len += ret;
        }
    }
    
    // 添加空行，表示请求头结束
    if (len < (int)sizeof(buffer) - 5) {
        ret = snprintf(buffer + len, sizeof(buffer) - len, "\r\n");
        if (ret > 0 && ret < (int)(sizeof(buffer) - len)) {
            len += ret;
        }
    }
    
    // 最终检查缓冲区是否溢出
    if (len >= (int)sizeof(buffer)) {
        log_error("请求缓冲区溢出，长度: %d", len);
        close(server_sock);
        free(new_path);
        if (status_code) *status_code = 500;
        if (response_size) *response_size = 0;
        return -1;
    }
    
    // 发送请求头
    ssize_t total_written = 0;
    while (total_written < len) {
        ssize_t bytes_written = write(server_sock, buffer + total_written, len - total_written);
        if (bytes_written <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(1000);
                continue;
            }
            log_error("发送请求头失败: %s", strerror(errno));
            close(server_sock);
            free(new_path);
            if (status_code) *status_code = 500;
            if (response_size) *response_size = 0;
            return -1;
        }
        total_written += bytes_written;
    }
    
    // 发送请求体（如果有）
    if (request->body != NULL && request->body_length > 0) {
        size_t body_total_written = 0;
        while (body_total_written < request->body_length) {
            ssize_t bytes_written = write(server_sock, request->body + body_total_written, 
                                        request->body_length - body_total_written);
            if (bytes_written <= 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    usleep(1000);
                    continue;
                }
                log_error("发送请求体失败: %s", strerror(errno));
                close(server_sock);
                free(new_path);
                if (status_code) *status_code = 500;
                if (response_size) *response_size = 0;
                return -1;
            }
            body_total_written += bytes_written;
        }
    }
    
    // 释放路径内存
    free(new_path);
    new_path = NULL;
    
    // 使用poll来处理超时
    struct pollfd fds[2];
    fds[0].fd = server_sock;
    fds[0].events = POLLIN;
    fds[1].fd = client_sock;
    fds[1].events = POLLIN;
    
    // 转发响应
    int active = 1;
    int response_status_code = 200;  // 默认状态码
    size_t total_response_size = 0;  // 响应大小
    int first_chunk = 1;  // 标记是否是第一个数据块
    long content_length = 0;  // Content-Length头的值
    time_t start_time = time(NULL);
    
    while (active) {
        int poll_result = poll(fds, 2, 5000);  // 5秒超时
        
        if (poll_result <= 0) {
            if (poll_result == 0) {
                // 检查总超时时间
                if (time(NULL) - start_time > 30) {
                    log_error("上游服务器响应超时: %s", upstream_info);
                    close(server_sock);
                    send_upstream_error_page(client_sock, 504, "Gateway Timeout", upstream_info);
                    if (status_code) *status_code = 504;
                    if (response_size) *response_size = 1024;
                    return -1;
                }
                continue;
            } else {
                log_error("poll失败: %s", strerror(errno));
            }
            break;
        }
        
        // 检查服务器套接字
        if (fds[0].revents & POLLIN) {
            ssize_t bytes_read = read(server_sock, buffer, sizeof(buffer));
            if (bytes_read <= 0) {
                active = 0;  // 服务器关闭连接
            } else {
                // 如果是第一个数据块，尝试解析状态码和Content-Length
                if (first_chunk) {
                    buffer[bytes_read] = '\0';  // 确保字符串以null结尾
                    response_status_code = parse_response_status_code(buffer);
                    content_length = parse_content_length(buffer);
                    first_chunk = 0;
                }
                
                // 将数据转发给客户端
                ssize_t total_written = 0;
                while (total_written < bytes_read) {
                    ssize_t bytes_written = write(client_sock, buffer + total_written, bytes_read - total_written);
                    if (bytes_written <= 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            // 非阻塞写入，稍后重试
                            usleep(1000);
                            continue;
                        }
                        log_error("向客户端发送数据失败: %s", strerror(errno));
                        active = 0;
                        break;
                    }
                    total_written += bytes_written;
                }
                
                // 累加响应大小
                total_response_size += bytes_read;
            }
        }
        
        // 检查客户端套接字（如果客户端关闭连接，我们也应该关闭）
        if (fds[1].revents & (POLLIN | POLLHUP)) {
            ssize_t bytes_read = read(client_sock, buffer, sizeof(buffer));
            if (bytes_read <= 0) {
                active = 0;  // 客户端关闭连接
            }
        }
    }
    
    // 返回实际的状态码和响应大小
    if (status_code) *status_code = response_status_code;
    
    // 如果有Content-Length头，使用它作为响应大小
    // 否则使用实际传输的字节数（这可能包括响应头）
    if (response_size) {
        if (content_length > 0) {
            *response_size = content_length;
        } else {
            *response_size = total_response_size;
        }
    }
    
    close(server_sock);
    log_debug("proxy请求完成: %s, 状态码: %d, 响应大小: %zu", upstream_info, response_status_code, total_response_size);
    return 0;
}
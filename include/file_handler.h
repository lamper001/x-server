/**
 * 本地文件处理模块
 */

#ifndef FILE_HANDLER_H
#define FILE_HANDLER_H

#include "http.h"
#include "config.h"

/**
 * 处理本地文件请求
 * 
 * @param client_sock 客户端套接字
 * @param request HTTP请求
 * @param route 匹配的路由
 * @param status_code 指向状态码的指针，用于返回实际的HTTP状态码
 * @param response_size 指向响应大小的指针，用于返回响应包体大小
 * @return 成功返回0，失败返回非0值
 */
int handle_local_file(int client_sock, http_request_t *request, route_t *route, int *status_code, size_t *response_size);

/**
 * 获取文件的MIME类型
 * 
 * @param filename 文件名
 * @return MIME类型字符串
 */
const char *get_mime_type(const char *filename);

/**
 * 使用零拷贝方式发送文件（sendfile/mmap），自动降级为read/write
 * @param client_sock 客户端socket
 * @param file_fd 文件fd
 * @param file_size 文件大小
 * @param sent_bytes 实际发送字节数
 * @return 0=成功，-1=失败
 */
int sendfile_optimized(int client_sock, int file_fd, size_t file_size, size_t *sent_bytes);

#endif /* FILE_HANDLER_H */
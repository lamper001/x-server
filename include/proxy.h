/**
 * 代理转发模块
 */

#ifndef PROXY_H
#define PROXY_H

#include "http.h"
#include "config.h"

/**
 * 将请求转发到目标服务器
 * 
 * @param client_sock 客户端套接字
 * @param request HTTP请求
 * @param route 匹配的路由
 * @param status_code 指向状态码的指针，用于返回实际的HTTP状态码
 * @param response_size 指向响应大小的指针，用于返回响应包体大小
 * @return 成功返回0，失败返回非0值
 */
int proxy_request(int client_sock, http_request_t *request, route_t *route, int *status_code, size_t *response_size);

#endif /* PROXY_H */
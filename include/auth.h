/**
 * 认证模块头文件
 */

#ifndef AUTH_H
#define AUTH_H

#include "../include/http.h"
#include "../include/config.h"

// 认证结果结构体
typedef struct {
    int success;            // 认证是否成功
    char error_message[256]; // 错误信息
} auth_result_t;

/**
 * 从HTTP请求中获取认证token
 * 
 * @param request HTTP请求
 * @return token字符串，如果没有找到则返回NULL
 */
char *get_auth_token(http_request_t *request);

/**
 * 验证请求是否有效
 * 
 * @param request HTTP请求
 * @param route 匹配的路由
 * @param result 认证结果
 * @return 验证通过返回1，失败返回0
 */
int validate_request(http_request_t *request, route_t *route, auth_result_t *result);

/**
 * 验证token是否有效（用于简单token认证）
 * 
 * @param route 匹配的路由
 * @param token 认证token
 * @param result 认证结果
 * @return 验证通过返回1，失败返回0
 */
int validate_token(route_t *route, const char *token, auth_result_t *result);

#endif /* AUTH_H */

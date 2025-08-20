/**
 * 认证模块实现
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/auth.h"
#include "../include/http.h"
#include "../include/config.h"
#include "../include/oauth.h"
#include "../include/logger.h"

// 从HTTP请求中获取认证token
char *get_auth_token(http_request_t *request) {
    // 尝试从Authorization头获取token
    char *auth_header = get_header_value(request, "Authorization");
    if (auth_header != NULL) {
        // 检查是否是Bearer token
        if (strncasecmp(auth_header, "Bearer ", 7) == 0) {
            return auth_header + 7;  // 跳过"Bearer "前缀
        }
        return auth_header;
    }
    
    // 尝试从查询字符串获取token
    if (request->query_string != NULL) {
        char *query = strdup(request->query_string);
        char *token_param = strstr(query, "token=");
        if (token_param != NULL) {
            token_param += 6;  // 跳过"token="
            
            // 查找参数结束位置
            char *end = strchr(token_param, '&');
            if (end != NULL) {
                *end = '\0';
            }
            
            char *token = strdup(token_param);
            free(query);
            return token;
        }
        free(query);
    }
    
    return NULL;
}

// 验证请求是否有效
int validate_request(http_request_t *request, route_t *route, auth_result_t *result) {
    // 初始化认证结果
    result->success = 0;
    strcpy(result->error_message, "");
    
    if (route->auth_type == AUTH_NONE) {
        result->success = 1;
        return 1;  // 不需要认证
    }
    
    // 根据认证类型选择不同的验证方法
    switch (route->auth_type) {
        case AUTH_OAUTH:
            // 使用OAuth认证
            {
                int oauth_result = validate_oauth(request, route);
                if (!oauth_result) {
                    const char *error_msg = get_oauth_error_message();
                    strcpy(result->error_message, error_msg);
                    free_oauth_error_message(error_msg);
                } else {
                    result->success = 1;
                }
                return oauth_result;
            }
            
        case AUTH_NONE:
        default:
            // 无需认证或未知认证类型
            result->success = 1;
            return 1;
    }
}

// 验证token是否有效（用于简单token认证，已废弃）
int validate_token(route_t *route, const char *token, auth_result_t *result) {
    (void)route;  // avoid unused parameter warning
    (void)token;
    
    if (result != NULL) {
        strcpy(result->error_message, "Token认证已废弃，请使用OAuth认证");
    }
    
    log_warn("Token认证已废弃，请使用OAuth认证");
    return 0;
}
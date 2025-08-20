/**
 * OAuth认证模块头文件
 */

#ifndef OAUTH_H
#define OAUTH_H

#include "../include/http.h"
#include "../include/config.h"

// API认证配置结构体
typedef struct {
    char *app_key;             // 应用密钥
    char *app_secret;          // 应用密钥
    char **allowed_urls;       // 允许访问的URL列表
    int url_count;             // URL数量
    int rate_limit;            // 每分钟最大请求次数
} api_auth_config_t;

/**
 * 加载API认证配置文件
 * 
 * @param filename 配置文件路径
 * @return 配置结构体数组，失败返回NULL
 */
api_auth_config_t **load_api_auth_config(const char *filename, int *count);

/**
 * 查找匹配的API认证配置
 * 
 * @param configs 配置结构体数组
 * @param count 配置数量
 * @param app_key 应用密钥
 * @return 匹配的配置，未找到返回NULL
 */
api_auth_config_t *find_api_auth_config(api_auth_config_t **configs, int count, const char *app_key);

/**
 * 检查URL是否在允许列表中
 * 
 * @param config API认证配置
 * @param url 请求URL
 * @return 允许返回1，不允许返回0
 */
int is_url_allowed(api_auth_config_t *config, const char *url);

/**
 * 验证OAuth请求
 * 
 * @param request HTTP请求
 * @param route 匹配的路由
 * @return 验证通过返回1，失败返回0
 */
int validate_oauth(http_request_t *request, route_t *route);

/**
 * 获取最后一次OAuth验证失败的错误信息
 * 
 * @return 错误信息字符串，调用者需要使用free_oauth_error_message释放
 */
const char *get_oauth_error_message();

/**
 * 释放get_oauth_error_message返回的错误信息
 * 
 * @param error_message 由get_oauth_error_message返回的错误信息
 */
void free_oauth_error_message(const char *error_message);

/**
 * 释放API认证配置
 * 
 * @param configs 配置结构体数组
 * @param count 配置数量
 */
void free_api_auth_config(api_auth_config_t **configs, int count);

/**
 * 初始化OAuth配置，在服务器启动时调用
 * 
 * @return 成功返回0，失败返回-1
 */
int init_oauth_config();

/**
 * 重新加载OAuth配置，可以通过-s reload参数触发
 * 
 * @return 成功返回0，失败返回-1
 */
int reload_oauth_config();

#endif /* OAUTH_H */

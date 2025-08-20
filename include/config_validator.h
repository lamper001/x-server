/**
 * 配置验证和优化模块头文件
 */

#ifndef CONFIG_VALIDATOR_H
#define CONFIG_VALIDATOR_H

#include "config.h"

/**
 * 验证并优化配置
 * @param config 配置结构体指针
 * @return 成功返回0，失败返回-1
 */
int validate_and_optimize_config(config_t *config);

/**
 * 打印配置摘要
 * @param config 配置结构体指针
 */
void print_config_summary(const config_t *config);

/**
 * 检查系统资源限制
 * @param config 配置结构体指针
 * @return 成功返回0，失败返回-1
 */
int check_system_limits(const config_t *config);

/**
 * 生成优化建议
 * @param config 配置结构体指针
 */
void generate_optimization_suggestions(const config_t *config);

#endif // CONFIG_VALIDATOR_H
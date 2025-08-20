/**
 * 连接处理模块头文件
 */

#ifndef CONNECTION_H
#define CONNECTION_H

#include "event_loop.h"
#include "event_loop.h"
#include "config.h"
#include "memory_pool.h"

// 连接结构体（不透明类型）
typedef struct connection connection_t;

/**
 * 初始化连接管理模块
 * 
 * @param pool_size 内存池初始大小
 * @return 成功返回0，失败返回非0值
 */
int init_connection_manager(size_t pool_size);

/**
 * 清理连接管理模块
 */
void cleanup_connection_manager(void);

/**
 * 压缩连接内存池
 * 
 * @return 释放的内存块数量
 */
int compress_connection_pool(void);

/**
 * 创建连接（标准事件循环版本）
 * 
 * @param fd 连接套接字
 * @param loop 事件循环
 * @param config 服务器配置
 * @param client_addr 客户端地址（可选，如果为NULL则使用getpeername获取）
 * @return 连接指针，失败返回NULL
 */
connection_t *connection_create(int fd, event_loop_t *loop, config_t *config, struct sockaddr_in *client_addr);

/**
 * 创建连接（增强版事件循环版本）
 * 
 * @param fd 连接套接字
 * @param loop 增强版事件循环
 * @param config 服务器配置
 * @param client_addr 客户端地址（可选，如果为NULL则使用getpeername获取）
 * @return 连接指针，失败返回NULL
 */
connection_t *connection_create_enhanced(int fd, event_loop_t *loop, config_t *config, struct sockaddr_in *client_addr);

/**
 * 销毁连接
 * 
 * @param conn 连接指针
 */
void connection_destroy(connection_t *conn);

/**
 * 接受新连接的回调函数
 * 
 * @param server_fd 服务器套接字
 * @param arg 参数（事件循环指针）
 */
void accept_connection_callback(int server_fd, void *arg);

void connection_read_callback(int fd, void *arg);
void connection_write_callback(int fd, void *arg);

// 增强版连接回调函数（在worker_process.c中实现）
void enhanced_connection_callback(int client_fd, void *arg);

#endif /* CONNECTION_H */
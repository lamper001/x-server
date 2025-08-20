/**
 * 进程锁定和实例检查模块头文件
 * 防止多个服务器实例同时运行
 */

#ifndef PROCESS_LOCK_H
#define PROCESS_LOCK_H

#include <sys/types.h>
#include <netinet/in.h>

/**
 * 检查端口是否可用
 * @param port 要检查的端口号
 * @return 0表示可用，-1表示不可用
 */
int check_port_available(int port);

/**
 * 检查指定端口上是否有x-server进程在运行
 * @param port 要检查的端口号
 * @return 1表示有x-server在运行，0表示没有
 */
int check_xserver_on_port(int port);

/**
 * 创建并锁定PID文件
 * @param pid_file PID文件路径，NULL则使用默认路径
 * @param port 服务器端口号
 * @return 0表示成功，-1表示失败
 */
int create_pid_file(const char *pid_file, int port);

/**
 * 检查服务器实例是否已在运行
 * @param port 服务器端口号
 * @return 运行中的进程PID，0表示没有运行
 */
int check_server_running(int port);

/**
 * 释放PID文件锁
 */
void release_pid_file(void);

/**
 * 发送信号给运行中的服务器实例
 * @param port 服务器端口号
 * @param signal 要发送的信号
 * @return 0表示成功，-1表示失败
 */
int send_signal_to_running_server(int port, int signal);

/**
 * 全面的服务器启动前检查
 * @param port 服务器端口号
 * @return 0表示检查通过，-1表示检查失败
 */
int pre_start_check(int port);

#endif // PROCESS_LOCK_H
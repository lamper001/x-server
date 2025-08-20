/**
 * 进程锁定和实例检查模块
 * 防止多个服务器实例同时运行
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <signal.h>

#include "../include/process_lock.h"
#include "../include/logger.h"

// 全局PID文件描述符
static int g_pid_fd = -1;
static char g_pid_file_path[256] = {0};

/**
 * 检查端口是否被占用
 */
int check_port_available(int port) {
    int sock_fd;
    struct sockaddr_in addr;
    int result = 0;
    
    // 创建测试套接字
    sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        log_error("创建测试套接字失败: %s", strerror(errno));
        return -1;
    }
    
    // 设置地址
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    
    // 尝试绑定端口
    if (bind(sock_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        if (errno == EADDRINUSE) {
            log_warn("端口 %d 已被占用", port);
            result = -1;
        } else {
            log_error("测试端口 %d 绑定失败: %s", port, strerror(errno));
            result = -1;
        }
    } else {
        log_info("端口 %d 可用", port);
        result = 0;
    }
    
    close(sock_fd);
    return result;
}

/**
 * 检查指定端口上是否有x-server进程在运行
 */
int check_xserver_on_port(int port) {
    char cmd[256];
    FILE *fp;
    char line[512];
    int found = 0;
    
    // 使用netstat检查端口占用情况
    snprintf(cmd, sizeof(cmd), "netstat -tlnp 2>/dev/null | grep ':%d '", port);
    
    fp = popen(cmd, "r");
    if (fp == NULL) {
        log_warn("无法执行netstat命令检查端口占用");
        return 0;
    }
    
    while (fgets(line, sizeof(line), fp) != NULL) {
        // 检查是否包含x-server进程名
        if (strstr(line, "x-server") != NULL) {
            log_warn("发现x-server进程正在使用端口 %d: %s", port, line);
            found = 1;
            break;
        }
    }
    
    pclose(fp);
    return found;
}

/**
 * 创建并锁定PID文件
 */
int create_pid_file(const char *pid_file, int port) {
    struct flock fl;
    char pid_str[32];
    int len;
    
    // 构建PID文件路径
    if (pid_file != NULL) {
        strncpy(g_pid_file_path, pid_file, sizeof(g_pid_file_path) - 1);
    } else {
        snprintf(g_pid_file_path, sizeof(g_pid_file_path), 
                 "logs/x-server.%d.pid", port);
    }
    
    // 创建logs目录（如果不存在）
    mkdir("logs", 0755);
    
    // 打开PID文件
    g_pid_fd = open(g_pid_file_path, O_RDWR | O_CREAT, 0644);
    if (g_pid_fd < 0) {
        log_error("无法创建PID文件 %s: %s", g_pid_file_path, strerror(errno));
        return -1;
    }
    
    // 设置文件锁
    fl.l_type = F_WRLCK;    // 写锁
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;           // 锁定整个文件
    
    // 尝试获取锁（非阻塞）
    if (fcntl(g_pid_fd, F_SETLK, &fl) < 0) {
        if (errno == EACCES || errno == EAGAIN) {
            // 文件已被锁定，说明有其他实例在运行
            char existing_pid[32] = {0};
            lseek(g_pid_fd, 0, SEEK_SET);
            read(g_pid_fd, existing_pid, sizeof(existing_pid) - 1);
            
            log_error("x-server实例已在运行，PID: %s", existing_pid);
            fprintf(stderr, "错误: x-server实例已在运行 (PID: %s)\n", existing_pid);
            fprintf(stderr, "如果确认没有其他实例运行，请删除PID文件: %s\n", g_pid_file_path);
            
            close(g_pid_fd);
            g_pid_fd = -1;
            return -1;
        } else {
            log_error("锁定PID文件失败: %s", strerror(errno));
            close(g_pid_fd);
            g_pid_fd = -1;
            return -1;
        }
    }
    
    // 截断文件并写入当前PID
    if (ftruncate(g_pid_fd, 0) < 0) {
        log_error("截断PID文件失败: %s", strerror(errno));
        close(g_pid_fd);
        g_pid_fd = -1;
        return -1;
    }
    
    len = snprintf(pid_str, sizeof(pid_str), "%d\n", getpid());
    if (write(g_pid_fd, pid_str, len) != len) {
        log_error("写入PID文件失败: %s", strerror(errno));
        close(g_pid_fd);
        g_pid_fd = -1;
        return -1;
    }
    
    // 刷新到磁盘
    fsync(g_pid_fd);
    
    log_info("创建PID文件成功: %s (PID: %d)", g_pid_file_path, getpid());
    return 0;
}

/**
 * 检查服务器实例是否已在运行
 */
int check_server_running(int port) {
    char pid_file[256];
    int fd;
    struct flock fl;
    char pid_str[32] = {0};
    pid_t existing_pid;
    
    // 构建PID文件路径
    snprintf(pid_file, sizeof(pid_file), "logs/x-server.%d.pid", port);
    
    // 尝试打开PID文件
    fd = open(pid_file, O_RDONLY);
    if (fd < 0) {
        if (errno == ENOENT) {
            // PID文件不存在，没有实例运行
            return 0;
        } else {
            log_warn("无法打开PID文件 %s: %s", pid_file, strerror(errno));
            return 0;
        }
    }
    
    // 尝试获取读锁
    fl.l_type = F_RDLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;
    
    if (fcntl(fd, F_SETLK, &fl) < 0) {
        if (errno == EACCES || errno == EAGAIN) {
            // 文件被锁定，有实例在运行
            read(fd, pid_str, sizeof(pid_str) - 1);
            existing_pid = atoi(pid_str);
            
            // 验证进程是否真的存在
            if (kill(existing_pid, 0) == 0) {
                log_info("发现x-server实例正在运行，PID: %d", existing_pid);
                close(fd);
                return existing_pid;
            } else {
                // 进程不存在，可能是僵尸PID文件
                log_warn("PID文件存在但进程不存在，可能是僵尸PID文件: %s", pid_file);
                close(fd);
                unlink(pid_file);  // 删除僵尸PID文件
                return 0;
            }
        }
    }
    
    close(fd);
    return 0;
}

/**
 * 释放PID文件锁
 */
void release_pid_file(void) {
    if (g_pid_fd >= 0) {
        // 删除PID文件
        unlink(g_pid_file_path);
        
        // 关闭文件描述符（自动释放锁）
        close(g_pid_fd);
        g_pid_fd = -1;
        
        log_info("释放PID文件: %s", g_pid_file_path);
    }
}

/**
 * Send signal to running server实例
 */
int send_signal_to_running_server(int port, int signal) {
    pid_t server_pid = check_server_running(port);
    
    if (server_pid <= 0) {
        fprintf(stderr, "Found x-server instance running on port: %d\n", port);
        return -1;
    }
    
    if (kill(server_pid, signal) != 0) {
        if (errno == ESRCH) {
            fprintf(stderr, "x-server进程 %d 不存在，清理PID文件\n", server_pid);
            char pid_file[256];
            snprintf(pid_file, sizeof(pid_file), "logs/x-server.%d.pid", port);
            unlink(pid_file);
            return -1;
        } else {
            perror("发送信号失败");
            return -1;
        }
    }
    
    const char *signal_name = "UNKNOWN";
    switch (signal) {
        case SIGHUP: signal_name = "RELOAD"; break;
        case SIGTERM: signal_name = "STOP"; break;
        case SIGQUIT: signal_name = "QUIT"; break;
    }
    
    printf("已发送%s信号给x-server进程 %d (端口 %d)\n", signal_name, server_pid, port);
    return 0;
}

/**
 * 全面的服务器启动前检查
 * 注意：此函数只应在Master进程中调用一次，Worker process不应调用
 */
int pre_start_check(int port) {
    // 检查是否为Worker process，如果是则跳过检查
    const char *worker_id = getenv("WORKER_PROCESS_ID");
    if (worker_id != NULL) {
        // 这是Worker process，跳过启动前检查
        return 0;
    }
    
    log_info("开始服务器启动前检查，端口: %d", port);
    
    // 1. 检查是否已有实例在运行
    pid_t existing_pid = check_server_running(port);
    if (existing_pid > 0) {
        fprintf(stderr, "错误: x-server实例已在端口 %d 上运行 (PID: %d)\n", port, existing_pid);
        fprintf(stderr, "请使用以下命令管理现有实例:\n");
        fprintf(stderr, "  停止服务: x-server -s stop\n");
        fprintf(stderr, "  重载配置: x-server -s reload\n");
        fprintf(stderr, "  查看状态: ps aux | grep x-server\n");
        return -1;
    }
    
    // 2. 检查端口是否可用
    if (check_port_available(port) != 0) {
        fprintf(stderr, "错误: 端口 %d 不可用\n", port);
        fprintf(stderr, "请检查端口占用情况:\n");
        fprintf(stderr, "  lsof -i :%d\n", port);
        fprintf(stderr, "  netstat -tlnp | grep %d\n", port);
        return -1;
    }
    
    // 3. 检查是否有其他x-server进程占用此端口
    if (check_xserver_on_port(port) > 0) {
        fprintf(stderr, "错误: 发现其他x-server进程正在使用端口 %d\n", port);
        return -1;
    }
    
    // 4. 创建PID文件锁
    if (create_pid_file(NULL, port) != 0) {
        return -1;
    }
    
    log_info("服务器启动前检查通过，端口: %d", port);
    return 0;
}

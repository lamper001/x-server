/**
 * Master进程管理模块
 * 负责管理配置、监控Worker进程、处理信号
 */

#ifndef MASTER_PROCESS_H
#define MASTER_PROCESS_H

#include <sys/types.h>
#include <signal.h>
#include "config.h"

// Master进程状态
typedef enum {
    MASTER_STARTING,
    MASTER_RUNNING,
    MASTER_RELOADING,
    MASTER_STOPPING,
    MASTER_STOPPED
} master_state_t;

// Worker进程信息
typedef struct worker_process {
    pid_t pid;
    int status;
    time_t start_time;
    time_t last_heartbeat;
    int respawn_count;
    struct worker_process *next;
} worker_process_t;

// Master进程上下文
typedef struct master_context {
    master_state_t state;
    config_t *config;
    worker_process_t *workers;
    int worker_count;
    int listen_fd;
    char *config_file;
    pid_t master_pid;
    
    // 统计信息
    time_t start_time;
    int total_workers_spawned;
    int config_reload_count;
} master_context_t;

/**
 * 初始化Master进程
 * 
 * @param config_file 配置文件路径
 * @param listen_port 监听端口
 * @return 成功返回0，失败返回-1
 */
int master_process_init(const char *config_file, int listen_port);

/**
 * 启动Master进程主循环
 * 
 * @return 成功返回0，失败返回-1
 */
int master_process_run(void);

/**
 * 创建Worker进程
 * 
 * @param worker_id Worker进程ID
 * @return 成功返回worker进程PID，失败返回-1
 */
pid_t spawn_worker_process(int worker_id);

/**
 * 监控Worker进程状态
 */
void monitor_worker_processes(void);

/**
 * 重新加载配置
 * 
 * @return 成功返回0，失败返回-1
 */
int reload_configuration(void);

/**
 * 优雅关闭所有Worker进程
 */
void shutdown_workers_gracefully(void);

/**
 * 强制终止所有Worker进程
 */
void terminate_workers_forcefully(void);

/**
 * 获取Master进程统计信息
 * 
 * @return Master上下文指针
 */
master_context_t *get_master_context(void);

#endif /* MASTER_PROCESS_H */
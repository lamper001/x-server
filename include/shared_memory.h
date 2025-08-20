/**
 * 共享内存管理模块
 * 用于Master进程和Worker进程之间的数据共享
 */

#ifndef SHARED_MEMORY_H
#define SHARED_MEMORY_H

#include <sys/types.h>
#include <stdint.h>
#include "config.h"

// 共享内存段标识
#define SHM_CONFIG_KEY    0x12345678
#define SHM_STATS_KEY     0x12345679
#define SHM_WORKERS_KEY   0x1234567A

// 共享统计信息结构
typedef struct shared_stats {
    uint64_t total_requests;        // 总请求数
    uint64_t total_bytes_sent;      // 总发送字节数
    uint64_t total_bytes_received;  // 总接收字节数
    uint32_t active_connections;    // 当前活跃连接数
    uint32_t total_connections;     // 总连接数
    time_t start_time;              // 服务器启动时间
    uint32_t worker_count;          // Worker进程数量
    
    // 每个Worker的统计信息
    struct {
        pid_t pid;
        uint64_t requests;
        uint64_t bytes_sent;
        uint64_t bytes_received;
        uint32_t active_connections;
        time_t start_time;
        time_t last_update;
    } workers[32];  // 最多支持32个Worker进程
} shared_stats_t;

// 共享配置信息结构
typedef struct shared_config {
    int version;                    // 配置版本号
    time_t update_time;            // 配置更新时间
    config_t config;               // 实际配置数据
} shared_config_t;

/**
 * 初始化共享内存
 * 
 * @return 成功返回0，失败返回-1
 */
int init_shared_memory(void);

/**
 * 清理共享内存
 */
void cleanup_shared_memory(void);

/**
 * 更新共享内存中的配置
 * 
 * @param config 新配置
 * @return 成功返回0，失败返回-1
 */
int update_shared_config(config_t *config);

/**
 * 从共享内存获取配置
 * 
 * @return 配置指针，失败返回NULL
 */
config_t *get_shared_config(void);

/**
 * 更新Worker进程统计信息
 * 
 * @param worker_id Worker进程ID
 * @param pid 进程PID
 * @param requests 处理的请求数
 * @param bytes_sent 发送字节数
 * @param bytes_received 接收字节数
 * @param active_connections 活跃连接数
 * @return 成功返回0，失败返回-1
 */
int update_worker_stats(int worker_id, pid_t pid, uint64_t requests, 
                       uint64_t bytes_sent, uint64_t bytes_received, 
                       uint32_t active_connections);

/**
 * 获取共享统计信息
 * 
 * @return 统计信息指针，失败返回NULL
 */
shared_stats_t *get_shared_stats(void);

/**
 * 复制配置结构体
 * 
 * @param config 源配置
 * @return 新配置指针，失败返回NULL
 */
config_t *duplicate_config(config_t *config);

#endif /* SHARED_MEMORY_H */
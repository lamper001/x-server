/**
 * 共享内存管理模块实现
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/shm.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <errno.h>
#include <time.h>

#include "../include/shared_memory.h"
#include "../include/logger.h"

// 共享内存段ID
static int g_config_shm_id = -1;
static int g_stats_shm_id = -1;

// 共享内存指针
static shared_config_t *g_shared_config = NULL;
static shared_stats_t *g_shared_stats = NULL;

// 信号量ID（用于同步访问）
static int g_config_sem_id = -1;
static int g_stats_sem_id = -1;

// 信号量操作结构
static struct sembuf sem_lock = {0, -1, SEM_UNDO};
static struct sembuf sem_unlock = {0, 1, SEM_UNDO};

/**
 * 创建或获取信号量
 */
static int create_semaphore(key_t key) {
    int sem_id = semget(key, 1, IPC_CREAT | 0666);
    if (sem_id == -1) {
        log_error("创建信号量失败: %s", strerror(errno));
        return -1;
    }
    
    // 初始化信号量值为1
    union semun {
        int val;
        struct semid_ds *buf;
        unsigned short *array;
    } sem_union;
    
    sem_union.val = 1;
    if (semctl(sem_id, 0, SETVAL, sem_union) == -1) {
        log_error("初始化信号量失败: %s", strerror(errno));
        return -1;
    }
    
    return sem_id;
}

/**
 * 获取信号量锁
 */
static int lock_semaphore(int sem_id) {
    if (semop(sem_id, &sem_lock, 1) == -1) {
        log_error("获取信号量锁失败: %s", strerror(errno));
        return -1;
    }
    return 0;
}

/**
 * 释放信号量锁
 */
static int unlock_semaphore(int sem_id) {
    if (semop(sem_id, &sem_unlock, 1) == -1) {
        log_error("释放信号量锁失败: %s", strerror(errno));
        return -1;
    }
    return 0;
}

/**
 * 初始化共享内存
 */
int init_shared_memory(void) {
    // 创建配置共享内存段
    g_config_shm_id = shmget(SHM_CONFIG_KEY, sizeof(shared_config_t), 
                            IPC_CREAT | 0666);
    if (g_config_shm_id == -1) {
        log_error("创建配置共享内存段失败: %s", strerror(errno));
        return -1;
    }
    
    // 附加配置共享内存
    g_shared_config = (shared_config_t *)shmat(g_config_shm_id, NULL, 0);
    if (g_shared_config == (void *)-1) {
        log_error("附加配置共享内存失败: %s", strerror(errno));
        return -1;
    }
    
    // 创建统计共享内存段
    g_stats_shm_id = shmget(SHM_STATS_KEY, sizeof(shared_stats_t), 
                           IPC_CREAT | 0666);
    if (g_stats_shm_id == -1) {
        log_error("创建统计共享内存段失败: %s", strerror(errno));
        shmdt(g_shared_config);
        return -1;
    }
    
    // 附加统计共享内存
    g_shared_stats = (shared_stats_t *)shmat(g_stats_shm_id, NULL, 0);
    if (g_shared_stats == (void *)-1) {
        log_error("附加统计共享内存失败: %s", strerror(errno));
        shmdt(g_shared_config);
        return -1;
    }
    
    // 创建信号量
    g_config_sem_id = create_semaphore(SHM_CONFIG_KEY + 1000);
    if (g_config_sem_id == -1) {
        shmdt(g_shared_config);
        shmdt(g_shared_stats);
        return -1;
    }
    
    g_stats_sem_id = create_semaphore(SHM_STATS_KEY + 1000);
    if (g_stats_sem_id == -1) {
        shmdt(g_shared_config);
        shmdt(g_shared_stats);
        return -1;
    }
    
    // 初始化共享内存内容
    if (lock_semaphore(g_config_sem_id) == 0) {
        memset(g_shared_config, 0, sizeof(shared_config_t));
        g_shared_config->version = 0;
        g_shared_config->update_time = time(NULL);
        unlock_semaphore(g_config_sem_id);
    }
    
    if (lock_semaphore(g_stats_sem_id) == 0) {
        memset(g_shared_stats, 0, sizeof(shared_stats_t));
        g_shared_stats->start_time = time(NULL);
        unlock_semaphore(g_stats_sem_id);
    }
    
    log_info("共享内存初始化成功");
    return 0;
}

/**
 * 清理共享内存
 */
void cleanup_shared_memory(void) {
    // 分离共享内存
    if (g_shared_config != NULL && g_shared_config != (void *)-1) {
        shmdt(g_shared_config);
        g_shared_config = NULL;
    }
    
    if (g_shared_stats != NULL && g_shared_stats != (void *)-1) {
        shmdt(g_shared_stats);
        g_shared_stats = NULL;
    }
    
    // 删除共享内存段
    if (g_config_shm_id != -1) {
        shmctl(g_config_shm_id, IPC_RMID, NULL);
        g_config_shm_id = -1;
    }
    
    if (g_stats_shm_id != -1) {
        shmctl(g_stats_shm_id, IPC_RMID, NULL);
        g_stats_shm_id = -1;
    }
    
    // 删除信号量
    if (g_config_sem_id != -1) {
        semctl(g_config_sem_id, 0, IPC_RMID);
        g_config_sem_id = -1;
    }
    
    if (g_stats_sem_id != -1) {
        semctl(g_stats_sem_id, 0, IPC_RMID);
        g_stats_sem_id = -1;
    }
    
    log_info("共享内存清理完成");
}


/**
 * 更新共享内存中的配置
 */
int update_shared_config(config_t *config) {
    if (g_shared_config == NULL || config == NULL) {
        log_error("共享配置内存未初始化或配置为空");
        return -1;
    }
    
    if (lock_semaphore(g_config_sem_id) != 0) {
        return -1;
    }
    
    // 更新配置版本和时间
    g_shared_config->version++;
    g_shared_config->update_time = time(NULL);
    
    // 复制配置内容（注意：这里只复制基本配置，不包括动态分配的内存）
    memcpy(&g_shared_config->config, config, sizeof(config_t));
    
    // 对于路由等动态内容，需要特殊处理
    // 这里简化处理，实际应用中可能需要更复杂的序列化机制
    if (config->route_count > 0 && config->route_count <= MAX_ROUTES) {
        for (int i = 0; i < config->route_count; i++) {
            // 复制路由基本信息
            g_shared_config->config.routes[i] = config->routes[i];
            
            // 复制字符串（限制长度以适应共享内存）
            if (strlen(config->routes[i].path_prefix) > 0) {
                strncpy(g_shared_config->config.routes[i].path_prefix, 
                       config->routes[i].path_prefix, 255);
                g_shared_config->config.routes[i].path_prefix[255] = '\0';
            }
            
            if (strlen(config->routes[i].target_host) > 0) {
                strncpy(g_shared_config->config.routes[i].target_host, 
                       config->routes[i].target_host, 255);
                g_shared_config->config.routes[i].target_host[255] = '\0';
            }
            
            if (strlen(config->routes[i].local_path) > 0) {
                strncpy(g_shared_config->config.routes[i].local_path, 
                       config->routes[i].local_path, 511);
                g_shared_config->config.routes[i].local_path[511] = '\0';
            }
            
            if (strlen(config->routes[i].charset) > 0) {
                strncpy(g_shared_config->config.routes[i].charset, 
                       config->routes[i].charset, 31);
                g_shared_config->config.routes[i].charset[31] = '\0';
            }
        }
    }
    
    unlock_semaphore(g_config_sem_id);
    
    log_info("共享配置更新成功，版本: %d", g_shared_config->version);
    return 0;
}

/**
 * 从共享内存获取配置
 */
config_t *get_shared_config(void) {
    if (g_shared_config == NULL) {
        log_error("共享配置内存未初始化");
        return NULL;
    }
    
    if (lock_semaphore(g_config_sem_id) != 0) {
        return NULL;
    }
    
    // 复制配置
    config_t *config = duplicate_config(&g_shared_config->config);
    
    unlock_semaphore(g_config_sem_id);
    
    return config;
}

/**
 * 更新Worker process统计信息
 */
int update_worker_stats(int worker_id, pid_t pid, uint64_t requests, 
                       uint64_t bytes_sent, uint64_t bytes_received, 
                       uint32_t active_connections) {
    if (g_shared_stats == NULL || worker_id < 0 || worker_id >= 32) {
        return -1;
    }
    
    if (lock_semaphore(g_stats_sem_id) != 0) {
        return -1;
    }
    
    // 更新Worker统计信息
    g_shared_stats->workers[worker_id].pid = pid;
    g_shared_stats->workers[worker_id].requests = requests;
    g_shared_stats->workers[worker_id].bytes_sent = bytes_sent;
    g_shared_stats->workers[worker_id].bytes_received = bytes_received;
    g_shared_stats->workers[worker_id].active_connections = active_connections;
    g_shared_stats->workers[worker_id].last_update = time(NULL);
    
    // 更新全局统计信息
    g_shared_stats->total_requests = 0;
    g_shared_stats->total_bytes_sent = 0;
    g_shared_stats->total_bytes_received = 0;
    g_shared_stats->active_connections = 0;
    
    for (int i = 0; i < 32; i++) {
        if (g_shared_stats->workers[i].pid > 0) {
            g_shared_stats->total_requests += g_shared_stats->workers[i].requests;
            g_shared_stats->total_bytes_sent += g_shared_stats->workers[i].bytes_sent;
            g_shared_stats->total_bytes_received += g_shared_stats->workers[i].bytes_received;
            g_shared_stats->active_connections += g_shared_stats->workers[i].active_connections;
        }
    }
    
    unlock_semaphore(g_stats_sem_id);
    
    return 0;
}

/**
 * 获取共享统计信息
 */
shared_stats_t *get_shared_stats(void) {
    return g_shared_stats;
}
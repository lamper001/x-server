/**
 * Shared Memory Management Module Implementation
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

// Shared memory segment IDs
static int g_config_shm_id = -1;
static int g_stats_shm_id = -1;

// Shared memory pointers
static shared_config_t *g_shared_config = NULL;
static shared_stats_t *g_shared_stats = NULL;

// Semaphore IDs (for synchronized access)
static int g_config_sem_id = -1;
static int g_stats_sem_id = -1;

// Semaphore operation structures
static struct sembuf sem_lock = {0, -1, SEM_UNDO};
static struct sembuf sem_unlock = {0, 1, SEM_UNDO};

/**
 * Create or get semaphore
 */
static int create_semaphore(key_t key) {
    int sem_id = semget(key, 1, IPC_CREAT | 0666);
    if (sem_id == -1) {
        log_error("Failed to create semaphore: %s", strerror(errno));
        return -1;
    }
    
    // Initialize semaphore value to 1
    union semun {
        int val;
        struct semid_ds *buf;
        unsigned short *array;
    } sem_union;
    
    sem_union.val = 1;
    if (semctl(sem_id, 0, SETVAL, sem_union) == -1) {
        log_error("Failed to initialize semaphore: %s", strerror(errno));
        return -1;
    }
    
    return sem_id;
}

/**
 * Acquire semaphore lock
 */
static int lock_semaphore(int sem_id) {
    if (semop(sem_id, &sem_lock, 1) == -1) {
        log_error("Failed to acquire semaphore lock: %s", strerror(errno));
        return -1;
    }
    return 0;
}

/**
 * Release semaphore lock
 */
static int unlock_semaphore(int sem_id) {
    if (semop(sem_id, &sem_unlock, 1) == -1) {
        log_error("Failed to release semaphore lock: %s", strerror(errno));
        return -1;
    }
    return 0;
}

/**
 * Initialize shared memory
 */
int init_shared_memory(void) {
    // Create configuration shared memory segment
    g_config_shm_id = shmget(SHM_CONFIG_KEY, sizeof(shared_config_t), 
                            IPC_CREAT | 0666);
    if (g_config_shm_id == -1) {
        log_error("Failed to create configuration shared memory segment: %s", strerror(errno));
        return -1;
    }
    
    // Attach configuration shared memory
    g_shared_config = (shared_config_t *)shmat(g_config_shm_id, NULL, 0);
    if (g_shared_config == (void *)-1) {
        log_error("Failed to attach configuration shared memory: %s", strerror(errno));
        return -1;
    }
    
    // Create statistics shared memory segment
    g_stats_shm_id = shmget(SHM_STATS_KEY, sizeof(shared_stats_t), 
                           IPC_CREAT | 0666);
    if (g_stats_shm_id == -1) {
        log_error("Failed to create statistics shared memory segment: %s", strerror(errno));
        shmdt(g_shared_config);
        return -1;
    }
    
    // Attach statistics shared memory
    g_shared_stats = (shared_stats_t *)shmat(g_stats_shm_id, NULL, 0);
    if (g_shared_stats == (void *)-1) {
        log_error("Failed to attach statistics shared memory: %s", strerror(errno));
        shmdt(g_shared_config);
        return -1;
    }
    
    // Create semaphores
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
    
    // Initialize shared memory content
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
    
    log_info("Shared memory initialized successfully");
    return 0;
}

/**
 * Clean up shared memory
 */
void cleanup_shared_memory(void) {
    // Detach shared memory
    if (g_shared_config != NULL && g_shared_config != (void *)-1) {
        shmdt(g_shared_config);
        g_shared_config = NULL;
    }
    
    if (g_shared_stats != NULL && g_shared_stats != (void *)-1) {
        shmdt(g_shared_stats);
        g_shared_stats = NULL;
    }
    
    // Delete shared memory segments
    if (g_config_shm_id != -1) {
        shmctl(g_config_shm_id, IPC_RMID, NULL);
        g_config_shm_id = -1;
    }
    
    if (g_stats_shm_id != -1) {
        shmctl(g_stats_shm_id, IPC_RMID, NULL);
        g_stats_shm_id = -1;
    }
    
    // Delete semaphores
    if (g_config_sem_id != -1) {
        semctl(g_config_sem_id, 0, IPC_RMID);
        g_config_sem_id = -1;
    }
    
    if (g_stats_sem_id != -1) {
        semctl(g_stats_sem_id, 0, IPC_RMID);
        g_stats_sem_id = -1;
    }
    
    log_info("Shared memory cleanup completed");
}


/**
 * Update configuration in shared memory
 */
int update_shared_config(config_t *config) {
    if (g_shared_config == NULL || config == NULL) {
        log_error("Shared configuration memory not initialized or configuration is empty");
        return -1;
    }
    
    if (lock_semaphore(g_config_sem_id) != 0) {
        return -1;
    }
    
    // Update configuration version and time
    g_shared_config->version++;
    g_shared_config->update_time = time(NULL);
    
    // Copy configuration content (Note: only copy basic configuration, excluding dynamically allocated memory)
    memcpy(&g_shared_config->config, config, sizeof(config_t));
    
    // For dynamic content like routes, special handling is required
    // Simplified handling here, actual applications may need more complex serialization mechanisms
    if (config->route_count > 0 && config->route_count <= MAX_ROUTES) {
        for (int i = 0; i < config->route_count; i++) {
            // Copy route basic information
            g_shared_config->config.routes[i] = config->routes[i];
            
            // Copy strings (limit length to fit in shared memory)
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
    
    log_info("Shared configuration updated successfully, version: %d", g_shared_config->version);
    return 0;
}

/**
 * Get configuration from shared memory
 */
config_t *get_shared_config(void) {
    if (g_shared_config == NULL) {
        log_error("Shared configuration memory not initialized");
        return NULL;
    }
    
    if (lock_semaphore(g_config_sem_id) != 0) {
        return NULL;
    }
    
    // Copy configuration
    config_t *config = duplicate_config(&g_shared_config->config);
    
    unlock_semaphore(g_config_sem_id);
    
    return config;
}

/**
 * Update Worker process statistics
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
    
    // Update Worker statistics
    g_shared_stats->workers[worker_id].pid = pid;
    g_shared_stats->workers[worker_id].requests = requests;
    g_shared_stats->workers[worker_id].bytes_sent = bytes_sent;
    g_shared_stats->workers[worker_id].bytes_received = bytes_received;
    g_shared_stats->workers[worker_id].active_connections = active_connections;
    g_shared_stats->workers[worker_id].last_update = time(NULL);
    
    // Update global statistics
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
 * Get shared statistics
 */
shared_stats_t *get_shared_stats(void) {
    return g_shared_stats;
}
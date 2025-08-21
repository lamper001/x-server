/**
 * Connection Pool Optimization Module Implementation
 * Phase 4: Connection Processing Optimization
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <pthread.h>
#include <assert.h>

#include "../include/connection_pool.h"
#include "../include/connection.h"
#include "../include/logger.h"
#include "../include/memory_pool.h"

// Forward declaration to avoid circular dependency
struct connection;

// Connection pool internal structure
struct connection_pool {
    connection_pool_config_t config;           // Connection pool configuration
    connection_pool_stats_t stats;             // Statistics
    
    // Connection management
    connection_t **connections;                // Connection array
    int connection_count;                      // Current connection count
    int connection_capacity;                   // Connection array capacity
    
    // Idle connection management
    connection_t **idle_connections;           // Idle connection array
    int idle_count;                           // Idle connection count
    int idle_capacity;                        // Idle connection array capacity
    
    // Thread safety
    pthread_mutex_t pool_mutex;               // Connection pool lock
    pthread_mutex_t idle_mutex;               // Idle connection lock
    pthread_mutex_t stats_mutex;              // Statistics lock
    
    // Cleanup thread
    pthread_t cleanup_thread;                 // Cleanup thread
    int cleanup_running;                      // Cleanup thread running flag
    
    // Memory pool
    void *memory_pool;                        // Memory pool
};

// Connection pool cleanup thread function
static void *cleanup_thread_func(void *arg) {
    connection_pool_t *pool = (connection_pool_t *)arg;
    
    while (pool->cleanup_running) {
        sleep(pool->config.pool_cleanup_interval);
        
        if (pool->cleanup_running) {
            connection_pool_cleanup_idle(pool);
        }
    }
    
    return NULL;
}

// Create connection pool
connection_pool_t *connection_pool_create(const connection_pool_config_t *config) {
    connection_pool_t *pool = malloc(sizeof(connection_pool_t));
    if (!pool) {
        log_error("Failed to create connection pool: memory allocation failed");
        return NULL;
    }
    
    // Initialize configuration
    memcpy(&pool->config, config, sizeof(connection_pool_config_t));
    
    // Initialize statistics
    memset(&pool->stats, 0, sizeof(connection_pool_stats_t));
    
    // Initialize connection array
    pool->connection_capacity = config->max_connections;
    pool->connections = calloc(pool->connection_capacity, sizeof(connection_t*));
    if (!pool->connections) {
        log_error("Failed to create connection pool: connection array allocation failed");
        free(pool);
        return NULL;
    }
    pool->connection_count = 0;
    
    // Initialize idle connection array
    pool->idle_capacity = config->max_idle_connections;
    pool->idle_connections = calloc(pool->idle_capacity, sizeof(connection_t*));
    if (!pool->idle_connections) {
        log_error("Failed to create connection pool: idle connection array allocation failed");
        free(pool->connections);
        free(pool);
        return NULL;
    }
    pool->idle_count = 0;
    
    // Initialize locks
    if (pthread_mutex_init(&pool->pool_mutex, NULL) != 0) {
        log_error("Failed to create connection pool: failed to initialize pool lock");
        free(pool->idle_connections);
        free(pool->connections);
        free(pool);
        return NULL;
    }
    
    if (pthread_mutex_init(&pool->idle_mutex, NULL) != 0) {
        log_error("Failed to create connection pool: failed to initialize idle connection lock");
        pthread_mutex_destroy(&pool->pool_mutex);
        free(pool->idle_connections);
        free(pool->connections);
        free(pool);
        return NULL;
    }
    
    if (pthread_mutex_init(&pool->stats_mutex, NULL) != 0) {
        log_error("Failed to create connection pool: failed to initialize statistics lock");
        pthread_mutex_destroy(&pool->idle_mutex);
        pthread_mutex_destroy(&pool->pool_mutex);
        free(pool->idle_connections);
        free(pool->connections);
        free(pool);
        return NULL;
    }
    
    // Initialize memory pool - use standard memory pool functions
    pool->memory_pool = create_memory_pool(1024 * 1024); // 1MB
    if (!pool->memory_pool) {
        log_warn("Failed to create connection pool memory pool, using system memory allocation");
    }
    
    // Start cleanup thread
    pool->cleanup_running = 1;
    if (pthread_create(&pool->cleanup_thread, NULL, cleanup_thread_func, pool) != 0) {
        log_error("Failed to create connection pool: failed to start cleanup thread");
        pthread_mutex_destroy(&pool->stats_mutex);
        pthread_mutex_destroy(&pool->idle_mutex);
        pthread_mutex_destroy(&pool->pool_mutex);
        if (pool->memory_pool) {
            destroy_memory_pool(pool->memory_pool);
        }
        free(pool->idle_connections);
        free(pool->connections);
        free(pool);
        return NULL;
    }
    
    log_info("Connection pool created successfully: max connections=%d, max idle connections=%d", 
             config->max_connections, config->max_idle_connections);
    
    return pool;
}

// Destroy connection pool
void connection_pool_destroy(connection_pool_t *pool) {
    if (!pool) {
        return;
    }
    
    // Stop cleanup thread
    pool->cleanup_running = 0;
    pthread_join(pool->cleanup_thread, NULL);
    
    // Close all connections
    pthread_mutex_lock(&pool->pool_mutex);
    for (int i = 0; i < pool->connection_count; i++) {
        if (pool->connections[i]) {
            connection_destroy(pool->connections[i]);
            pool->connections[i] = NULL;
        }
    }
    pthread_mutex_unlock(&pool->pool_mutex);
    
    // Destroy locks
    pthread_mutex_destroy(&pool->stats_mutex);
    pthread_mutex_destroy(&pool->idle_mutex);
    pthread_mutex_destroy(&pool->pool_mutex);
    
    // Destroy memory pool
    if (pool->memory_pool) {
        destroy_memory_pool(pool->memory_pool);
    }
    
    // Free arrays
    free(pool->idle_connections);
    free(pool->connections);
    
    log_info("Connection pool destruction completed");
    
    free(pool);
}

// Get connection from connection pool
connection_t *connection_pool_get_connection(connection_pool_t *pool, int fd, 
                                           void *loop, int is_enhanced_loop,
                                           config_t *config, struct sockaddr_in *client_addr) {
    if (!pool || !config) {
        return NULL;
    }
    
    connection_t *conn = NULL;
    
    // Try to get from idle connection pool
    if (pool->config.enable_connection_reuse) {
        pthread_mutex_lock(&pool->idle_mutex);
        
        if (pool->idle_count > 0) {
            // Get the last idle connection
            conn = pool->idle_connections[--pool->idle_count];
            pool->idle_connections[pool->idle_count] = NULL;
            
            // Update statistics
            pthread_mutex_lock(&pool->stats_mutex);
            atomic_fetch_add(&pool->stats.reused_connections, 1);
            atomic_fetch_add(&pool->stats.idle_connections, -1);
            atomic_fetch_add(&pool->stats.active_connections, 1);
            pthread_mutex_unlock(&pool->stats_mutex);
            
            log_debug("Reused idle connection: fd=%d", (int)(long)conn);
        }
        
        pthread_mutex_unlock(&pool->idle_mutex);
    }
    
    // If no reusable connection, create new connection
    if (!conn) {
        pthread_mutex_lock(&pool->pool_mutex);
        
        // Check connection limit
        if (pool->connection_count >= pool->config.max_connections) {
            log_warn("Connection pool is full, cannot create new connection: current=%d, max=%d", 
                     pool->connection_count, pool->config.max_connections);
            pthread_mutex_unlock(&pool->pool_mutex);
            return NULL;
        }
        
        // Create new connection
        if (is_enhanced_loop) {
            conn = connection_create_enhanced(fd, (event_loop_t*)loop, config, client_addr);
        } else {
            conn = connection_create(fd, (event_loop_t*)loop, config, client_addr);
        }
        
        if (conn) {
            // Add to connection array
            pool->connections[pool->connection_count++] = conn;
            
            // Update statistics
            pthread_mutex_lock(&pool->stats_mutex);
            atomic_fetch_add(&pool->stats.created_connections, 1);
            atomic_fetch_add(&pool->stats.total_connections, 1);
            atomic_fetch_add(&pool->stats.active_connections, 1);
            pthread_mutex_unlock(&pool->stats_mutex);
            
            log_debug("Created new connection: fd=%d, current connections=%d, enhanced=%d", fd, pool->connection_count, is_enhanced_loop);
        }
        
        pthread_mutex_unlock(&pool->pool_mutex);
    }
    
    return conn;
}

// Return connection to connection pool
void connection_pool_return_connection(connection_pool_t *pool, connection_t *conn) {
    if (!pool || !conn) {
        return;
    }
    
    // Check if connection can be reused - simplified logic for now
    if (pool->config.enable_connection_reuse && 
        pool->idle_count < pool->config.max_idle_connections) {
        
        pthread_mutex_lock(&pool->idle_mutex);
        
        if (pool->idle_count < pool->idle_capacity) {
            pool->idle_connections[pool->idle_count++] = conn;
            
            // Update statistics
            pthread_mutex_lock(&pool->stats_mutex);
            atomic_fetch_add(&pool->stats.idle_connections, 1);
            atomic_fetch_add(&pool->stats.active_connections, -1);
            pthread_mutex_unlock(&pool->stats_mutex);
            
            log_debug("Connection returned to idle pool: conn=%p, idle connections=%d", conn, pool->idle_count);
        } else {
            // Idle pool is full, close connection directly
            connection_pool_close_connection(pool, conn);
        }
        
        pthread_mutex_unlock(&pool->idle_mutex);
    } else {
        // Cannot reuse, close directly
        connection_pool_close_connection(pool, conn);
    }
}

// Close connection (remove from pool)
void connection_pool_close_connection(connection_pool_t *pool, connection_t *conn) {
    if (!pool || !conn) {
        return;
    }
    
    pthread_mutex_lock(&pool->pool_mutex);
    
    // Remove from connection array
    for (int i = 0; i < pool->connection_count; i++) {
        if (pool->connections[i] == conn) {
            // Move subsequent connections
            for (int j = i; j < pool->connection_count - 1; j++) {
                pool->connections[j] = pool->connections[j + 1];
            }
            pool->connections[--pool->connection_count] = NULL;
            break;
        }
    }
    
    // Remove from idle connection array
    pthread_mutex_lock(&pool->idle_mutex);
    for (int i = 0; i < pool->idle_count; i++) {
        if (pool->idle_connections[i] == conn) {
            // Move subsequent connections
            for (int j = i; j < pool->idle_count - 1; j++) {
                pool->idle_connections[j] = pool->idle_connections[j + 1];
            }
            pool->idle_connections[--pool->idle_count] = NULL;
            break;
        }
    }
    pthread_mutex_unlock(&pool->idle_mutex);
    
    // Update statistics
    pthread_mutex_lock(&pool->stats_mutex);
    atomic_fetch_add(&pool->stats.closed_connections, 1);
    atomic_fetch_add(&pool->stats.active_connections, -1);
    pthread_mutex_unlock(&pool->stats_mutex);
    
    pthread_mutex_unlock(&pool->pool_mutex);
    
    // Destroy connection
    connection_destroy(conn);
    
    log_debug("Connection closed: conn=%p, current connections=%d", conn, pool->connection_count);
}

// Get connection pool statistics
void connection_pool_get_stats(connection_pool_t *pool, connection_pool_stats_t *stats) {
    if (!pool || !stats) {
        return;
    }
    
    pthread_mutex_lock(&pool->stats_mutex);
    memcpy(stats, &pool->stats, sizeof(connection_pool_stats_t));
    pthread_mutex_unlock(&pool->stats_mutex);
}

// Reset connection pool statistics
void connection_pool_reset_stats(connection_pool_t *pool) {
    if (!pool) {
        return;
    }
    
    pthread_mutex_lock(&pool->stats_mutex);
    memset(&pool->stats, 0, sizeof(connection_pool_stats_t));
    pthread_mutex_unlock(&pool->stats_mutex);
}

// Print connection pool statistics
void connection_pool_print_stats(connection_pool_t *pool) {
    if (!pool) {
        return;
    }
    
    connection_pool_stats_t stats;
    connection_pool_get_stats(pool, &stats);
    
    log_info("=== Connection Pool Statistics ===");
    log_info("Total connections: %d", atomic_load(&stats.total_connections));
    log_info("Active connections: %d", atomic_load(&stats.active_connections));
    log_info("Idle connections: %d", atomic_load(&stats.idle_connections));
    log_info("Reused connections: %d", atomic_load(&stats.reused_connections));
    log_info("Created connections: %d", atomic_load(&stats.created_connections));
    log_info("Closed connections: %d", atomic_load(&stats.closed_connections));
    log_info("Timeout connections: %d", atomic_load(&stats.timeout_connections));
    log_info("Total requests: %lu", stats.total_requests);
    log_info("Total bytes read: %lu", stats.total_bytes_read);
    log_info("Total bytes written: %lu", stats.total_bytes_written);
    log_info("Average connection lifetime: %.2f seconds", stats.avg_connection_lifetime);
    log_info("Average requests per connection: %.2f", stats.avg_requests_per_conn);
    log_info("======================");
}

// Clean up idle connections
int connection_pool_cleanup_idle(connection_pool_t *pool) {
    if (!pool) {
        return 0;
    }
    
    int cleaned = 0;
    time_t now = time(NULL);
    
    pthread_mutex_lock(&pool->idle_mutex);
    
    for (int i = pool->idle_count - 1; i >= 0; i--) {
        connection_t *conn = pool->idle_connections[i];
        if (conn && (now - 0) > pool->config.idle_timeout) { // Simplified for now, using fixed time
            // Remove timed out idle connections
            for (int j = i; j < pool->idle_count - 1; j++) {
                pool->idle_connections[j] = pool->idle_connections[j + 1];
            }
            pool->idle_connections[--pool->idle_count] = NULL;
            
            // Close connection
            connection_pool_close_connection(pool, conn);
            cleaned++;
            
            log_debug("Cleaned up timed out idle connection: conn=%p, idle time=%ld seconds", 
                      conn, now - 0);
        }
    }
    
    pthread_mutex_unlock(&pool->idle_mutex);
    
    if (cleaned > 0) {
        log_info("Connection pool cleanup completed: cleaned %d timed out idle connections", cleaned);
    }
    
    return cleaned;
}

// Set connection pool configuration
int connection_pool_set_config(connection_pool_t *pool, const connection_pool_config_t *config) {
    if (!pool || !config) {
        return -1;
    }
    
    pthread_mutex_lock(&pool->pool_mutex);
    memcpy(&pool->config, config, sizeof(connection_pool_config_t));
    pthread_mutex_unlock(&pool->pool_mutex);
    
    log_info("Connection pool configuration updated");
    return 0;
}

// Get connection pool configuration
void connection_pool_get_config(connection_pool_t *pool, connection_pool_config_t *config) {
    if (!pool || !config) {
        return;
    }
    
    pthread_mutex_lock(&pool->pool_mutex);
    memcpy(config, &pool->config, sizeof(connection_pool_config_t));
    pthread_mutex_unlock(&pool->pool_mutex);
}

// Load connection pool configuration from Config file
connection_pool_config_t connection_pool_load_config(const config_t *config) {
    connection_pool_config_t pool_config = {0};
    
    if (!config) {
        // Use default configuration
        pool_config.max_connections = 10000;
        pool_config.min_idle_connections = 10;
        pool_config.max_idle_connections = 1000;
        pool_config.connection_timeout = 30;
        pool_config.idle_timeout = 60;
        pool_config.keepalive_timeout = 30;
        pool_config.max_requests_per_conn = 1000;
        pool_config.enable_connection_reuse = 1;
        pool_config.enable_connection_pooling = 1;
        pool_config.pool_cleanup_interval = 30;
        return pool_config;
    }
    
    // Load from server configuration
    pool_config.max_connections = config->max_connections;
    pool_config.min_idle_connections = config->worker_connections / 10; // 10% as minimum idle
    pool_config.max_idle_connections = config->worker_connections / 2;  // 50% as maximum idle
    pool_config.connection_timeout = config->connection_timeout;
    pool_config.idle_timeout = config->keepalive_timeout * 2; // Idle timeout is 2x keepalive timeout
    pool_config.keepalive_timeout = config->keepalive_timeout;
    pool_config.max_requests_per_conn = 1000; // Default value
    pool_config.enable_connection_reuse = 1;   // Enable by default
    pool_config.enable_connection_pooling = 1; // Enable by default
    pool_config.pool_cleanup_interval = 30;    // Clean up every 30 seconds
    
    return pool_config;
} 
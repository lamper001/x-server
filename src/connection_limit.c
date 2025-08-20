/**
 * Connection Limit Module - Reference nginx implementation
 * Provides IP-level connection count and request rate limiting
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <arpa/inet.h>
#include "../include/logger.h"
#include "../include/connection_limit.h"

// Global limit configuration
static connection_limit_config_t g_limit_config = {
    .max_connections_per_ip = 10,
    .max_requests_per_second = 10,
    .max_requests_burst = 20,
    .cleanup_interval = 60,
    .enable_connection_limit = 1,
    .enable_rate_limit = 1
};

// IP connection tracking table
static ip_connection_t *g_ip_connections[IP_HASH_SIZE];
static pthread_mutex_t g_connections_mutex = PTHREAD_MUTEX_INITIALIZER;

// IP request rate tracking table
static ip_rate_limit_t *g_ip_rates[IP_HASH_SIZE];
static pthread_mutex_t g_rates_mutex = PTHREAD_MUTEX_INITIALIZER;

// Last cleanup time
static time_t g_last_cleanup = 0;

// Calculate IP hash value
static unsigned int ip_hash(const char *ip) {
    unsigned int hash = 5381;
    for (int i = 0; ip[i]; i++) {
        hash = ((hash << 5) + hash) + ip[i];
    }
    return hash % IP_HASH_SIZE;
}

// Get or create IP connection record
static ip_connection_t *get_or_create_ip_connection(const char *ip) {
    unsigned int hash = ip_hash(ip);
    ip_connection_t *conn = g_ip_connections[hash];
    
    // Find existing record
    while (conn) {
        if (strcmp(conn->ip, ip) == 0) {
            return conn;
        }
        conn = conn->next;
    }
    
    // Create new record
    conn = malloc(sizeof(ip_connection_t));
    if (!conn) {
        log_error("Failed to allocate IP connection record memory");
        return NULL;
    }
    
    strncpy(conn->ip, ip, sizeof(conn->ip) - 1);
    conn->ip[sizeof(conn->ip) - 1] = '\0';
    conn->connection_count = 0;
    conn->last_access = time(NULL);
    conn->next = g_ip_connections[hash];
    g_ip_connections[hash] = conn;
    
    return conn;
}

// Get or create IP rate limit record
static ip_rate_limit_t *get_or_create_ip_rate(const char *ip) {
    unsigned int hash = ip_hash(ip);
    ip_rate_limit_t *rate = g_ip_rates[hash];
    
    // Find existing record
    while (rate) {
        if (strcmp(rate->ip, ip) == 0) {
            return rate;
        }
        rate = rate->next;
    }
    
    // Create new record
    rate = malloc(sizeof(ip_rate_limit_t));
    if (!rate) {
        log_error("Failed to allocate IP rate record memory");
        return NULL;
    }
    
    strncpy(rate->ip, ip, sizeof(rate->ip) - 1);
    rate->ip[sizeof(rate->ip) - 1] = '\0';
    rate->request_count = 0;
    rate->burst_count = 0;
    rate->last_request = time(NULL);
    rate->window_start = time(NULL);
    rate->next = g_ip_rates[hash];
    g_ip_rates[hash] = rate;
    
    return rate;
}

// Clean up expired records
static void cleanup_expired_records(void) {
    time_t now = time(NULL);
    
    // If time since last cleanup is less than cleanup interval, skip
    if (now - g_last_cleanup < g_limit_config.cleanup_interval) {
        return;
    }
    
    g_last_cleanup = now;
    
    // Clean up connection records
    pthread_mutex_lock(&g_connections_mutex);
    for (int i = 0; i < IP_HASH_SIZE; i++) {
        ip_connection_t **conn_ptr = &g_ip_connections[i];
        while (*conn_ptr) {
            ip_connection_t *conn = *conn_ptr;
            // Clean up records with no activity for 60 seconds
            if (now - conn->last_access > 60 && conn->connection_count == 0) {
                *conn_ptr = conn->next;
                free(conn);
            } else {
                conn_ptr = &conn->next;
            }
        }
    }
    pthread_mutex_unlock(&g_connections_mutex);
    
    // Clean up rate records
    pthread_mutex_lock(&g_rates_mutex);
    for (int i = 0; i < IP_HASH_SIZE; i++) {
        ip_rate_limit_t **rate_ptr = &g_ip_rates[i];
        while (*rate_ptr) {
            ip_rate_limit_t *rate = *rate_ptr;
            // Clean up records with no activity for 5 minutes
            if (now - rate->last_request > 300) {
                *rate_ptr = rate->next;
                free(rate);
            } else {
                rate_ptr = &rate->next;
            }
        }
    }
    pthread_mutex_unlock(&g_rates_mutex);
    
    log_debug("Completed expired record cleanup");
}

// Check connection limit
int check_connection_limit(const char *client_ip) {
    if (!g_limit_config.enable_connection_limit) {
        return 0; // Connection limit disabled
    }
    
    if (!client_ip) {
        return -1;
    }
    
    cleanup_expired_records();
    
    pthread_mutex_lock(&g_connections_mutex);
    
    ip_connection_t *conn = get_or_create_ip_connection(client_ip);
    if (!conn) {
        pthread_mutex_unlock(&g_connections_mutex);
        return -1;
    }
    
    // Check connection count limit
    if (conn->connection_count >= g_limit_config.max_connections_per_ip) {
        pthread_mutex_unlock(&g_connections_mutex);
        log_warn("IP %s connection count exceeded: %d >= %d", 
                client_ip, conn->connection_count, g_limit_config.max_connections_per_ip);
        return -1;
    }
    
    // Increment connection count
    conn->connection_count++;
    conn->last_access = time(NULL);
    
    pthread_mutex_unlock(&g_connections_mutex);
    
    log_debug("IP %s current connection count: %d", client_ip, conn->connection_count);
    return 0;
}

// Release connection
void release_connection(const char *client_ip) {
    if (!g_limit_config.enable_connection_limit || !client_ip) {
        return;
    }
    
    pthread_mutex_lock(&g_connections_mutex);
    
    unsigned int hash = ip_hash(client_ip);
    ip_connection_t *conn = g_ip_connections[hash];
    
    while (conn) {
        if (strcmp(conn->ip, client_ip) == 0) {
            if (conn->connection_count > 0) {
                conn->connection_count--;
            }
            conn->last_access = time(NULL);
            log_debug("IP %s released connection, current connection count: %d", client_ip, conn->connection_count);
            break;
        }
        conn = conn->next;
    }
    
    pthread_mutex_unlock(&g_connections_mutex);
}

// Check request rate limit
int check_rate_limit(const char *client_ip) {
    if (!g_limit_config.enable_rate_limit) {
        return 0; // Rate limit disabled
    }
    
    if (!client_ip) {
        return -1;
    }
    
    cleanup_expired_records();
    
    pthread_mutex_lock(&g_rates_mutex);
    
    ip_rate_limit_t *rate = get_or_create_ip_rate(client_ip);
    if (!rate) {
        pthread_mutex_unlock(&g_rates_mutex);
        return -1;
    }
    
    time_t now = time(NULL);
    
    // Check if need to reset count window (reset every second)
    if (now > rate->window_start) {
        rate->request_count = 0;
        rate->window_start = now;
    }
    
    // Check basic rate limit
    if (rate->request_count >= g_limit_config.max_requests_per_second) {
        // Check burst limit
        if (rate->burst_count >= g_limit_config.max_requests_burst) {
            pthread_mutex_unlock(&g_rates_mutex);
            log_warn("IP %s request rate exceeded: %d req/s, burst: %d", 
                    client_ip, rate->request_count, rate->burst_count);
            return -1;
        }
        rate->burst_count++;
    }
    
    // Increment request count
    rate->request_count++;
    rate->last_request = now;
    
    // Burst count decay (decrease by 1 per second)
    if (now > rate->last_request + 1 && rate->burst_count > 0) {
        rate->burst_count--;
    }
    
    pthread_mutex_unlock(&g_rates_mutex);
    
    log_debug("IP %s request rate: %d req/s, burst: %d", 
             client_ip, rate->request_count, rate->burst_count);
    return 0;
}

// Get IP connection statistics
int get_ip_connection_stats(const char *client_ip, ip_connection_stats_t *stats) {
    if (!client_ip || !stats) {
        return -1;
    }
    
    memset(stats, 0, sizeof(ip_connection_stats_t));
    
    pthread_mutex_lock(&g_connections_mutex);
    
    unsigned int hash = ip_hash(client_ip);
    ip_connection_t *conn = g_ip_connections[hash];
    
    while (conn) {
        if (strcmp(conn->ip, client_ip) == 0) {
            stats->connection_count = conn->connection_count;
            stats->last_access = conn->last_access;
            break;
        }
        conn = conn->next;
    }
    
    pthread_mutex_unlock(&g_connections_mutex);
    
    pthread_mutex_lock(&g_rates_mutex);
    
    hash = ip_hash(client_ip);
    ip_rate_limit_t *rate = g_ip_rates[hash];
    
    while (rate) {
        if (strcmp(rate->ip, client_ip) == 0) {
            stats->request_count = rate->request_count;
            stats->burst_count = rate->burst_count;
            stats->last_request = rate->last_request;
            break;
        }
        rate = rate->next;
    }
    
    pthread_mutex_unlock(&g_rates_mutex);
    
    return 0;
}

// Configure connection limit
void configure_connection_limit(const connection_limit_config_t *config) {
    if (config) {
        g_limit_config = *config;
        log_info("Connection limit configuration updated: max_connections=%d, max_request_rate=%d/s", 
                g_limit_config.max_connections_per_ip, 
                g_limit_config.max_requests_per_second);
    }
}

// Update connection limit configuration (from server configuration)
void update_connection_limit_from_config(int max_connections_per_ip, int cleanup_interval) {
    g_limit_config.max_connections_per_ip = max_connections_per_ip > 0 ? max_connections_per_ip : 1000;
    g_limit_config.cleanup_interval = cleanup_interval > 0 ? cleanup_interval : 120;
    
    log_info("Update connection limit configuration: max_connections_per_ip=%d, cleanup_interval=%d", 
             g_limit_config.max_connections_per_ip, g_limit_config.cleanup_interval);
}

// Get current configuration
void get_connection_limit_config(connection_limit_config_t *config) {
    if (config) {
        *config = g_limit_config;
    }
}

// Get global statistics
void get_global_limit_stats(global_limit_stats_t *stats) {
    if (!stats) {
        return;
    }
    
    memset(stats, 0, sizeof(global_limit_stats_t));
    
    // Count connection information
    pthread_mutex_lock(&g_connections_mutex);
    for (int i = 0; i < IP_HASH_SIZE; i++) {
        ip_connection_t *conn = g_ip_connections[i];
        while (conn) {
            stats->total_tracked_ips++;
            stats->total_connections += conn->connection_count;
            conn = conn->next;
        }
    }
    pthread_mutex_unlock(&g_connections_mutex);
    
    // Count rate information
    pthread_mutex_lock(&g_rates_mutex);
    for (int i = 0; i < IP_HASH_SIZE; i++) {
        ip_rate_limit_t *rate = g_ip_rates[i];
        while (rate) {
            stats->total_requests += rate->request_count;
            if (rate->burst_count > 0) {
                stats->total_burst_requests += rate->burst_count;
            }
            rate = rate->next;
        }
    }
    pthread_mutex_unlock(&g_rates_mutex);
}

// Clean up all limit records
void cleanup_all_limits(void) {
    // Clean up connection records
    pthread_mutex_lock(&g_connections_mutex);
    for (int i = 0; i < IP_HASH_SIZE; i++) {
        ip_connection_t *conn = g_ip_connections[i];
        while (conn) {
            ip_connection_t *next = conn->next;
            free(conn);
            conn = next;
        }
        g_ip_connections[i] = NULL;
    }
    pthread_mutex_unlock(&g_connections_mutex);
    
    // Clean up rate records
    pthread_mutex_lock(&g_rates_mutex);
    for (int i = 0; i < IP_HASH_SIZE; i++) {
        ip_rate_limit_t *rate = g_ip_rates[i];
        while (rate) {
            ip_rate_limit_t *next = rate->next;
            free(rate);
            rate = next;
        }
        g_ip_rates[i] = NULL;
    }
    pthread_mutex_unlock(&g_rates_mutex);
    
    log_info("All connection limit records cleaned up");
}
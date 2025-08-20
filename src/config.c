/**
 * Configuration Management Module Implementation - Simplified Version
 * Only supports new format gateway_multiprocess.conf config file
 * API authentication uses separate api_auth.conf file
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>

#include "../include/config.h"
#include "../include/logger.h"

// Get CPU core count
static int get_cpu_count(void) {
#ifdef _SC_NPROCESSORS_ONLN
    long cpu_count = sysconf(_SC_NPROCESSORS_ONLN);
    if (cpu_count > 0) {
        return (int)cpu_count;
    }
#endif
    
    // If detection fails, return default value
    return 14;  // Consistent with 10k configuration
}

// Parse size value (supports K, M, G suffixes)
static size_t parse_size_value(const char *value) {
    if (value == NULL) return 0;
    
    char *endptr;
    long long size = strtoll(value, &endptr, 10);
    
    if (size < 0) return 0;
    
    if (endptr && *endptr) {
        switch (tolower(*endptr)) {
            case 'k':
                size *= 1024;
                break;
            case 'm':
                size *= 1024 * 1024;
                break;
            case 'g':
                size *= 1024 * 1024 * 1024;
                break;
        }
    }
    
    return (size_t)size;
}

// Parse route configuration line
static void parse_route_line(const char *line, route_t *route) {
    if (line == NULL || route == NULL) return;
    
    char *line_copy = strdup(line);
    if (line_copy == NULL) return;
    
    // Initialize route
    memset(route, 0, sizeof(route_t));
    route->auth_type = AUTH_NONE;
    strncpy(route->charset, "utf-8", MAX_CHARSET_LEN - 1);
    route->charset[MAX_CHARSET_LEN - 1] = '\0';
    
    char *token = strtok(line_copy, " \t");
    int field = 0;
    
    while (token != NULL && field < 5) {
        switch (field) {
            case 0: // Route type
                if (strcmp(token, "static") == 0) {
                    route->type = ROUTE_STATIC;
                } else if (strcmp(token, "proxy") == 0) {
                    route->type = ROUTE_PROXY;
                } else {
                    route->type = ROUTE_STATIC; // Default static
                }
                break;
                
            case 1: // Path prefix
                if (strlen(token) >= sizeof(route->path_prefix)) {
                    log_error("Path prefix too long: %s", token);
                    break;
                }
                strncpy(route->path_prefix, token, sizeof(route->path_prefix) - 1);
                route->path_prefix[sizeof(route->path_prefix) - 1] = '\0';
                break;
                
            case 2: // Target
                if (route->type == ROUTE_STATIC) {
                    if (strlen(token) >= sizeof(route->local_path)) {
                        log_error("Local path too long: %s", token);
                        break;
                    }
                    strncpy(route->local_path, token, sizeof(route->local_path) - 1);
                    route->local_path[sizeof(route->local_path) - 1] = '\0';
                } else {
                    // Parse host:port
                    char *colon = strchr(token, ':');
                    if (colon != NULL) {
                        size_t host_len = colon - token;
                        if (host_len >= sizeof(route->target_host)) {
                            log_error("Target hostname too long: %s", token);
                            break;
                        }
                        *colon = '\0';
                        strncpy(route->target_host, token, sizeof(route->target_host) - 1);
                        route->target_host[sizeof(route->target_host) - 1] = '\0';
                        
                        int port = atoi(colon + 1);
                        if (port <= 0 || port > 65535) {
                            log_error("Invalid port number: %d", port);
                            route->target_port = 80;
                        } else {
                            route->target_port = port;
                        }
                    } else {
                        if (strlen(token) >= sizeof(route->target_host)) {
                            log_error("Target hostname too long: %s", token);
                            break;
                        }
                        strncpy(route->target_host, token, sizeof(route->target_host) - 1);
                        route->target_host[sizeof(route->target_host) - 1] = '\0';
                        route->target_port = 80; // Default port
                    }
                }
                break;
                
            case 3: // Authentication type
                if (strcmp(token, "oauth") == 0) {
                    route->auth_type = AUTH_OAUTH;
                } else if (strcmp(token, "none") == 0) {
                    route->auth_type = AUTH_NONE;
                } else {
                    log_warn("Unknown authentication type: %s, using default value none", token);
                    route->auth_type = AUTH_NONE; // Default no authentication
                }
                break;
                
            case 4: // Character set
                if (strlen(token) >= sizeof(route->charset)) {
                    log_error("Character set name too long: %s", token);
                    break;
                }
                strncpy(route->charset, token, sizeof(route->charset) - 1);
                route->charset[sizeof(route->charset) - 1] = '\0';
                break;
        }
        
        token = strtok(NULL, " \t");
        field++;
    }
    
    free(line_copy);
}

// Load main config file (only supports new format)
config_t *load_config(const char *filename) {
    FILE *file = fopen(filename, "r");
    if (file == NULL) {
        log_error("Unable to open config file: %s", filename);
        return NULL;
    }
    
    config_t *config = (config_t *)malloc(sizeof(config_t));
    if (config == NULL) {
        log_error("Failed to allocate configuration memory");
        fclose(file);
        return NULL;
    }
    
    // Initialize default configuration - 10K concurrency optimization default values (consistent with 10k_concurrency_multiprocess.conf)
    memset(config, 0, sizeof(config_t));
    
    // Auto-detect CPU core count, use default value if failed
    int cpu_count = get_cpu_count();
    config->worker_processes = (cpu_count > 0) ? cpu_count : 14;
    
    // Global configuration - multi-process 10K concurrency optimization
    config->worker_connections = 8192;
    config->listen_port = 9001;
    config->max_connections = config->worker_processes * config->worker_connections;
    config->keepalive_timeout = 30;
    config->max_request_size = 50 * 1024 * 1024;  // 50MB
    config->worker_rlimit_nofile = 1048576;
    
    // Network configuration - multi-process 10K concurrency optimization
    config->tcp_nodelay = 1;  // Enable TCP_NODELAY, reduce latency
    config->tcp_nopush = 0;  // Disable TCP_NOPUSH
    config->tcp_fastopen = 1;  // Enable TCP Fast Open
    config->reuseport = 1;  // Enable port reuse
    
    // Buffer configuration - multi-process 10K concurrency memory optimization
    config->client_header_buffer_size = 16 * 1024;  // 16KB request header buffer
    config->large_client_header_buffers = 32 * 16 * 1024;  // 32 16KB large request header buffers
    config->client_body_buffer_size = 1024 * 1024;  // 1MB request body buffer
    config->proxy_buffer_size = 16 * 1024;  // 16KB proxy buffer
    config->proxy_buffers = 16 * 16 * 1024;  // 16 16KB proxy buffer pool
    config->proxy_busy_buffers_size = 32 * 1024;  // 32KB busy buffer size
    
    // Timeout configuration - multi-process 10K concurrency connection management
    config->client_header_timeout = 30;  // Reduce request header timeout
    config->client_body_timeout = 30;  // Reduce request body timeout
    config->send_timeout = 30;  // Reduce send timeout
    config->proxy_connect_timeout = 15;  // Reduce proxy connection timeout
    config->proxy_send_timeout = 30;  // Reduce proxy send timeout
    config->proxy_read_timeout = 30;  // Reduce proxy read timeout
    
    // Multi-process 10K concurrency performance optimization configuration
    config->use_thread_pool = 1;  // Enable thread pool for CPU-intensive tasks
    config->thread_pool_size = 4;  // 4 threads per process, avoid excessive thread competition
    config->thread_pool_queue_size = 2000;  // Thread pool queue size
    
    // Event loop optimization - multi-process 10K concurrency core optimization
    config->event_loop_max_events = 50000;  // 50K events per process, total 500K events
    config->event_loop_timeout = 5;  // Event loop timeout (milliseconds)
    config->event_loop_batch_size = 2000;  // Event batch processing size
    
    // Memory pool optimization - multi-process 10K concurrency memory management
    config->memory_pool_size = 209715200;  // 100MB memory pool per process, total 1GB
    config->memory_block_size = 32768;  // Memory block size: 32KB
    config->memory_pool_segments = 32;  // Memory pool segment count
    config->memory_pool_cleanup_interval = 300;  // Memory pool cleanup interval (seconds)
    
    // Connection limit optimization - multi-process 10K concurrency connection management
    config->connection_limit_per_ip = 1000;  // Connection limit per IP
    config->connection_limit_window = 60;  // Connection limit time window (seconds)
    config->connection_timeout = 300;  // Connection timeout (seconds)
    config->connection_keepalive_max = 5000;  // Maximum Keep-alive connections
    
    config->route_count = 0;
    strncpy(config->log_config.log_path, "./logs", MAX_LOG_PATH_LEN - 1);
    config->log_config.log_path[MAX_LOG_PATH_LEN - 1] = '\0';
    config->log_config.log_daily = 1;
    config->log_config.log_level = LOG_LEVEL_WARN;  // WARN level to reduce log overhead
    
    // Add default route configuration
    if (config->route_count < MAX_ROUTES) {
        // Default static files route
        route_t *route = &config->routes[config->route_count];
        route->type = ROUTE_STATIC;
        strncpy(route->path_prefix, "/", MAX_PATH_PREFIX_LEN - 1);
        route->path_prefix[MAX_PATH_PREFIX_LEN - 1] = '\0';
        strncpy(route->local_path, "./public", MAX_LOCAL_PATH_LEN - 1);
        route->local_path[MAX_LOCAL_PATH_LEN - 1] = '\0';
        route->auth_type = AUTH_NONE;
        strncpy(route->charset, "utf-8", MAX_CHARSET_LEN - 1);
        route->charset[MAX_CHARSET_LEN - 1] = '\0';
        config->route_count++;
    }
    
    // Only output config loading logs in Master process
    if (getenv("WORKER_PROCESS_ID") == NULL) {
        log_info("Loading config file: %s", filename);
        log_info("Using new format config file parser");
    }
    
    // Parse config file
    char line[1024];
    while (fgets(line, sizeof(line), file)) {
        // Remove newline and whitespace characters
        char *trimmed = line;
        while (*trimmed && (*trimmed == ' ' || *trimmed == '\t')) trimmed++;
        if (*trimmed == '\0' || *trimmed == '#') continue;
        
        char *newline = strchr(trimmed, '\n');
        if (newline) *newline = '\0';
        
        // Check if it's a route configuration line
        if (strncmp(trimmed, "route ", 6) == 0) {
            // Parse route configuration: route <type> <path> <target> [auth] [charset]
            if (config->route_count < MAX_ROUTES) {
                parse_route_line(trimmed + 6, &config->routes[config->route_count]);
                config->route_count++;
            }
            continue;
        }
        
        char *key = strtok(trimmed, " \t");
        if (key == NULL) continue;
        char *value = strtok(NULL, " \t");
        if (value == NULL) continue;
        
        // Remove semicolon after value
        char *semicolon = strchr(value, ';');
        if (semicolon) {
            *semicolon = '\0';
        }
        
        if (strcmp(key, "worker_processes") == 0) {
            if (strcmp(value, "auto") == 0) {
                config->worker_processes = get_cpu_count();  // Auto-detect CPU core count
            } else {
                config->worker_processes = atoi(value);
            }
        }
        else if (strcmp(key, "worker_connections") == 0) {
            config->worker_connections = atoi(value);
            if (config->worker_connections <= 0) {
                config->worker_connections = 1024;
            }
        }
        else if (strcmp(key, "worker_rlimit_nofile") == 0) {
            config->worker_rlimit_nofile = atoi(value);
            if (config->worker_rlimit_nofile <= 0) {
                config->worker_rlimit_nofile = 65535;
            }
        }
        else if (strcmp(key, "listen_port") == 0) {
            config->listen_port = atoi(value);
            if (config->listen_port <= 0 || config->listen_port > 65535) {
                config->listen_port = 9001;
            }
        }
        else if (strcmp(key, "max_connections") == 0) {
            config->max_connections = atoi(value);
            if (config->max_connections <= 0) {
                config->max_connections = 10000;
            }
        }
        else if (strcmp(key, "keepalive_timeout") == 0) {
            config->keepalive_timeout = atoi(value);
            if (config->keepalive_timeout <= 0) {
                config->keepalive_timeout = 65;
            }
        }
        else if (strcmp(key, "client_max_body_size") == 0) {
            config->max_request_size = parse_size_value(value);
            if (config->max_request_size <= 0) {
                config->max_request_size = 10 * 1024 * 1024;  // Default 10MB
            }
        }
        else if (strcmp(key, "tcp_nodelay") == 0) {
            config->tcp_nodelay = (strcmp(value, "on") == 0) ? 1 : 0;
        }
        else if (strcmp(key, "tcp_nopush") == 0) {
            config->tcp_nopush = (strcmp(value, "on") == 0) ? 1 : 0;
        }
        else if (strcmp(key, "client_header_buffer_size") == 0) {
            config->client_header_buffer_size = parse_size_value(value);
            if (config->client_header_buffer_size <= 0) {
                config->client_header_buffer_size = 1024;  // Default 1KB
            }
        }
        else if (strcmp(key, "large_client_header_buffers") == 0) {
            config->large_client_header_buffers = parse_size_value(value);
        }
        // 10K concurrency optimization configuration items (consistent with 10k_concurrency_multiprocess.conf)
        else if (strcmp(key, "event_loop_max_events") == 0) {
            config->event_loop_max_events = atoi(value);
            if (config->event_loop_max_events <= 0) {
                config->event_loop_max_events = 50000;  // Default 50K
            }
        }
        else if (strcmp(key, "event_loop_timeout") == 0) {
            config->event_loop_timeout = atoi(value);
            if (config->event_loop_timeout <= 0) {
                config->event_loop_timeout = 10;  // Default 10ms
            }
        }
        else if (strcmp(key, "event_loop_batch_size") == 0) {
            config->event_loop_batch_size = atoi(value);
            if (config->event_loop_batch_size <= 0) {
                config->event_loop_batch_size = 2000;  // Default 2000
            }
        }
        else if (strcmp(key, "memory_pool_size") == 0) {
            config->memory_pool_size = parse_size_value(value);
            if (config->memory_pool_size <= 0) {
                config->memory_pool_size = 524288000;  // Default 500MB
            }
        }
        else if (strcmp(key, "memory_block_size") == 0) {
            config->memory_block_size = parse_size_value(value);
            if (config->memory_block_size <= 0) {
                config->memory_block_size = 32768;  // Default 32KB
            }
        }
        else if (strcmp(key, "memory_pool_segments") == 0) {
            config->memory_pool_segments = atoi(value);
            if (config->memory_pool_segments <= 0) {
                config->memory_pool_segments = 64;  // Default 64
            }
        }
        else if (strcmp(key, "memory_pool_cleanup_interval") == 0) {
            config->memory_pool_cleanup_interval = atoi(value);
            if (config->memory_pool_cleanup_interval <= 0) {
                config->memory_pool_cleanup_interval = 600;  // Default 600 seconds
            }
        }
        else if (strcmp(key, "connection_limit_per_ip") == 0) {
            config->connection_limit_per_ip = atoi(value);
            if (config->connection_limit_per_ip <= 0) {
                config->connection_limit_per_ip = 1000;  // Default 1000
            }
        }
        else if (strcmp(key, "connection_limit_window") == 0) {
            config->connection_limit_window = atoi(value);
            if (config->connection_limit_window <= 0) {
                config->connection_limit_window = 120;  // Default 120 seconds
            }
        }
        else if (strcmp(key, "connection_timeout") == 0) {
            config->connection_timeout = atoi(value);
            if (config->connection_timeout <= 0) {
                config->connection_timeout = 600;  // Default 600 seconds
            }
        }
        else if (strcmp(key, "connection_keepalive_max") == 0) {
            config->connection_keepalive_max = atoi(value);
            if (config->connection_keepalive_max <= 0) {
                config->connection_keepalive_max = 5000;  // Default 5000
            }
        }
        else if (strcmp(key, "use_thread_pool") == 0) {
            config->use_thread_pool = (strcmp(value, "on") == 0) ? 1 : 0;
        }
        else if (strcmp(key, "thread_pool_size") == 0) {
            config->thread_pool_size = atoi(value);
            if (config->thread_pool_size <= 0) {
                config->thread_pool_size = 8;  // Default 8
            }
        }
        else if (strcmp(key, "thread_pool_queue_size") == 0) {
            config->thread_pool_queue_size = atoi(value);
            if (config->thread_pool_queue_size <= 0) {
                config->thread_pool_queue_size = 5000;  // Default 5000
            }
        }
        else if (strcmp(key, "client_body_buffer_size") == 0) {
            config->client_body_buffer_size = parse_size_value(value);
            if (config->client_body_buffer_size <= 0) {
                config->client_body_buffer_size = 16384;  // Default 16KB
            }
        }
        else if (strcmp(key, "client_header_timeout") == 0) {
            config->client_header_timeout = atoi(value);
            if (config->client_header_timeout <= 0) {
                config->client_header_timeout = 60;  // Default 60 seconds
            }
        }
        else if (strcmp(key, "client_body_timeout") == 0) {
            config->client_body_timeout = atoi(value);
            if (config->client_body_timeout <= 0) {
                config->client_body_timeout = 60;  // Default 60 seconds
            }
        }
        else if (strcmp(key, "send_timeout") == 0) {
            config->send_timeout = atoi(value);
            if (config->send_timeout <= 0) {
                config->send_timeout = 60;  // Default 60 seconds
            }
        }
        else if (strcmp(key, "log_path") == 0) {
            if (strlen(value) >= sizeof(config->log_config.log_path)) {
                log_error("Log path too long: %s", value);
            } else {
                strncpy(config->log_config.log_path, value, sizeof(config->log_config.log_path) - 1);
                config->log_config.log_path[sizeof(config->log_config.log_path) - 1] = '\0';
            }
        }
        else if (strcmp(key, "log_daily") == 0) {
            config->log_config.log_daily = atoi(value);
        }
        else if (strcmp(key, "log_level") == 0) {
            config->log_config.log_level = atoi(value);
        }
    }
    
    fclose(file);
    
    log_info("Config file loading completed");
    log_info("Worker processes: %d", config->worker_processes);
    log_info("Listening port: %d", config->listen_port);
    log_info("Route count: %d", config->route_count);
    
    return config;
}

/**
 * Validate configuration validity
 */
int validate_config(config_t *config) {
    if (config == NULL) {
        log_error("Configuration is empty");
        return 0;
    }
    
    // Validate basic configuration
    if (config->worker_processes <= 0 || config->worker_processes > 64) {
        log_error("Invalid worker processes: %d (should be between 1-64)", config->worker_processes);
        return 0;
    }
    
    if (config->max_connections <= 0 || config->max_connections > 65536) {
        log_error("Invalid max connections: %d (should be between 1-65536)", config->max_connections);
        return 0;
    }
    
    if (config->keepalive_timeout < 0 || config->keepalive_timeout > 3600) {
        log_error("Invalid keepalive timeout: %d (should be between 0-3600 seconds)", config->keepalive_timeout);
        return 0;
    }
    
    if (config->client_max_body_size <= 0) {
        log_error("Invalid client max body size: %d", config->client_max_body_size);
        return 0;
    }
    
    if (config->max_request_size <= 0) {
        log_error("Invalid max request size: %zu", config->max_request_size);
        return 0;
    }
    
    // Validate route configuration
    if (config->route_count <= 0) {
        log_error("No routes configured");
        return 0;
    }
    
    for (int i = 0; i < config->route_count; i++) {
        route_t *route = &config->routes[i];
        
        if (strlen(route->path_prefix) == 0) {
            log_error("Route %d path prefix is empty", i + 1);
            return 0;
        }
        
        if (route->type == ROUTE_PROXY) {
            if (strlen(route->target_host) == 0) {
                log_error("Route %d target host is empty", i + 1);
                return 0;
            }
            if (route->target_port <= 0 || route->target_port > 65535) {
                log_error("Route %d target port invalid: %d", i + 1, route->target_port);
                return 0;
            }
        } else if (route->type == ROUTE_STATIC) {
            if (strlen(route->local_path) == 0) {
                log_error("Route %d local path is empty", i + 1);
                return 0;
            }
        }
    }
    
    // Validate log configuration
    if (strlen(config->log_config.log_path) == 0) {
        log_warn("Log path is empty, will use default path");
        strncpy(config->log_config.log_path, "/tmp/x-server.log", MAX_LOG_PATH_LEN - 1);
        config->log_config.log_path[MAX_LOG_PATH_LEN - 1] = '\0';
    }
    
    // Validate performance configuration
    if (config->worker_connections <= 0) {
        log_error("Invalid worker connections: %d", config->worker_connections);
        return 0;
    }
    
    if (config->worker_rlimit_nofile <= 0) {
        log_error("Invalid file descriptor limit: %d", config->worker_rlimit_nofile);
        return 0;
    }
    
    // Validate buffer configuration
    if (config->client_header_buffer_size <= 0) {
        log_error("Invalid request header buffer size: %d", config->client_header_buffer_size);
        return 0;
    }
    
    if (config->large_client_header_buffers <= 0) {
        log_error("Invalid large request header buffer count: %d", config->large_client_header_buffers);
        return 0;
    }
    
    if (config->client_body_buffer_size <= 0) {
        log_error("Invalid request body buffer size: %d", config->client_body_buffer_size);
        return 0;
    }
    
    // Validate timeout configuration
    if (config->client_header_timeout <= 0) {
        log_error("Invalid request header timeout: %d", config->client_header_timeout);
        return 0;
    }
    
    if (config->client_body_timeout <= 0) {
        log_error("Invalid request body timeout: %d", config->client_body_timeout);
        return 0;
    }
    
    if (config->send_timeout <= 0) {
        log_error("Invalid send timeout: %d", config->send_timeout);
        return 0;
    }
    
    log_info("Configuration validation passed");
    return 1;
}

// Free configuration structure
void free_config(config_t *config) {
    if (config != NULL) {
        free(config);
    }
}

// Find route
route_t *find_route(config_t *config, const char *path) {
    if (config == NULL || path == NULL) {
        return NULL;
    }
    
    // Find longest matching route
    route_t *best_match = NULL;
    size_t best_match_len = 0;
    
    for (int i = 0; i < config->route_count; i++) {
        route_t *route = &config->routes[i];
        size_t prefix_len = strlen(route->path_prefix);
        
        if (strncmp(path, route->path_prefix, prefix_len) == 0) {
            if (prefix_len > best_match_len) {
                best_match = route;
                best_match_len = prefix_len;
            }
        }
    }
    
    return best_match;
}

// Get default configuration - 10K concurrency optimization default values
config_t *get_default_config(void) {
    config_t *config = (config_t *)malloc(sizeof(config_t));
    if (config == NULL) {
        return NULL;
    }
    
    // Initialize default configuration - 10K concurrency optimization default values (consistent with 10k_concurrency_multiprocess.conf)
    memset(config, 0, sizeof(config_t));
    
    // Auto-detect CPU core count, use default value if failed
    config->worker_processes = get_cpu_count();
    if (config->worker_processes <= 0) {
        config->worker_processes = 14;  // Default 14 worker processes, consistent with 10k configuration
    }
    
    // Global configuration - multi-process 10K concurrency optimization (consistent with 10k_concurrency_multiprocess.conf)
    config->worker_connections = 8192;  // 8K connections per worker process, consistent with 10k configuration
    config->listen_port = 9001;
    config->max_connections = 112000;  // Total connections 112K, consistent with 10k configuration
    config->keepalive_timeout = 30;  // Reduce keepalive time, quickly release connections
    config->max_request_size = 50 * 1024 * 1024;  // 50MB, consistent with 10k configuration
    config->worker_rlimit_nofile = 1048576;  // File descriptor limit, match system limit
    
    // Network configuration - multi-process 10K concurrency optimization
    config->tcp_nodelay = 1;  // Enable TCP_NODELAY, reduce latency
    config->tcp_nopush = 0;  // Disable TCP_NOPUSH
    config->tcp_fastopen = 1;  // Enable TCP Fast Open
    config->reuseport = 1;  // Enable port reuse
    
    // Buffer configuration - multi-process 10K concurrency memory optimization
    config->client_header_buffer_size = 32 * 1024;  // 32KB request header buffer
    config->large_client_header_buffers = 32 * 16 * 1024;  // 32 16KB large request header buffers
    config->client_body_buffer_size = 1024 * 1024;  // 1MB request body buffer
    config->proxy_buffer_size = 32 * 1024;  // 32KB proxy buffer
    config->proxy_buffers = 32 * 16 * 1024;  // 32 16KB proxy buffer pool
    config->proxy_busy_buffers_size = 64 * 1024;  // 64KB busy buffer size
    
    // Timeout configuration - multi-process 10K concurrency connection management
    config->client_header_timeout = 30;  // Reduce request header timeout
    config->client_body_timeout = 30;  // Reduce request body timeout
    config->send_timeout = 30;  // Reduce send timeout
    config->proxy_connect_timeout = 15;  // Reduce proxy connection timeout
    config->proxy_send_timeout = 30;  // Reduce proxy send timeout
    config->proxy_read_timeout = 30;  // Reduce proxy read timeout
    
    // Multi-process 10K concurrency performance optimization configuration
    config->use_thread_pool = 1;  // Enable thread pool for CPU-intensive tasks
    config->thread_pool_size = 4;  // 4 threads per process, avoid excessive thread competition
    config->thread_pool_queue_size = 2000;  // Thread pool queue size
    
    // Event loop optimization - multi-process 10K concurrency core optimization
    config->event_loop_max_events = 20000;  // 20K events per process, total 200K events
    config->event_loop_timeout = 10;  // Event loop timeout (milliseconds)
    config->event_loop_batch_size = 2000;  // Event batch processing size
    
    // Memory pool optimization - multi-process 10K concurrency memory management
    config->memory_pool_size = 104857600;  // 100MB memory pool per process, total 1GB
    config->memory_block_size = 32768;  // Memory block size: 32KB
    config->memory_pool_segments = 32;  // Memory pool segment count
    config->memory_pool_cleanup_interval = 300;  // Memory pool cleanup interval (seconds)
    
    // Connection limit optimization - multi-process 10K concurrency connection management
    config->connection_limit_per_ip = 1000;  // Connection limit per IP
    config->connection_limit_window = 60;  // Connection limit time window (seconds)
    config->connection_timeout = 300;  // Connection timeout (seconds)
    config->connection_keepalive_max = 5000;  // Maximum Keep-alive connections
    
    config->route_count = 0;
    strncpy(config->log_config.log_path, "./logs", MAX_LOG_PATH_LEN - 1);
    config->log_config.log_path[MAX_LOG_PATH_LEN - 1] = '\0';
    config->log_config.log_daily = 1;
    config->log_config.log_level = LOG_LEVEL_WARN;  // WARN level to reduce log overhead
    
    // Add default route configuration
    if (config->route_count < MAX_ROUTES) {
        // Default static files route
        route_t *route = &config->routes[config->route_count];
        route->type = ROUTE_STATIC;
        strncpy(route->path_prefix, "/", MAX_PATH_PREFIX_LEN - 1);
        route->path_prefix[MAX_PATH_PREFIX_LEN - 1] = '\0';
        strncpy(route->local_path, "./public", MAX_LOCAL_PATH_LEN - 1);
        route->local_path[MAX_LOCAL_PATH_LEN - 1] = '\0';
        route->auth_type = AUTH_NONE;
        strncpy(route->charset, "utf-8", MAX_CHARSET_LEN - 1);
        route->charset[MAX_CHARSET_LEN - 1] = '\0';
        config->route_count++;
    }
    
    return config;
}

// Duplicate configuration structure (for multi-process)
config_t *duplicate_config(config_t *source) {
    if (source == NULL) {
        return NULL;
    }
    
    config_t *config = (config_t *)malloc(sizeof(config_t));
    if (config == NULL) {
        log_error("Failed to allocate configuration memory");
        return NULL;
    }
    
    // Directly copy entire structure
    memcpy(config, source, sizeof(config_t));
    
    return config;
}

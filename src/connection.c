/**
 * Connection Processing Module Implementation
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
#include <ctype.h>
#include <time.h>
#include <pthread.h>

#include "../include/connection.h"
#include "../include/event_loop.h"
#include "../include/http.h"
#include "../include/logger.h"
#include "../include/config.h"
#include "../include/proxy.h"
#include "../include/auth.h"
#include "../include/file_handler.h"
#include "../include/memory_pool.h"
#include "../include/worker_process.h"
#include "../include/connection_limit.h"

#define BUFFER_SIZE 8192
#define CONNECTION_POOL_SIZE (1024 * 1024 * 10)  // 10MB connection memory pool

// Thread-safe IP address conversion function
static char *safe_inet_ntoa(struct in_addr addr) {
    static __thread char ip_str[INET_ADDRSTRLEN];
    memset(ip_str, 0, sizeof(ip_str));
    if (inet_ntop(AF_INET, &addr, ip_str, sizeof(ip_str)) == NULL) {
        strcpy(ip_str, "0.0.0.0");
    }
    return ip_str;
}

// Connection memory pool
static memory_pool_t *connection_pool = NULL;

// Connection structure
struct connection {
    int fd;                     // Connection socket
    event_loop_t *loop;         // Unified event loop
    config_t *config;           // Server configuration
    char *read_buffer;          // Read buffer
    size_t read_size;           // Read buffer size
    size_t read_pos;            // Read buffer position
    char *write_buffer;         // Write buffer
    size_t write_size;          // Write buffer size
    size_t write_pos;           // Write buffer position
    http_request_t request;     // HTTP request
    int keep_alive;             // Whether to keep connection alive
    time_t last_activity;       // Last activity time
    int timeout;                // Timeout (seconds)
    struct sockaddr_in addr;    // Client address
};

// Forward declarations
void connection_read_callback(int fd, void *arg);
void connection_write_callback(int fd, void *arg);
static connection_t *connection_create_internal(int fd, config_t *config, struct sockaddr_in *client_addr);
static void connection_destroy_internal(connection_t *conn);

// Initialize connection management module
int init_connection_manager(size_t pool_size) {
    // If already initialized, return success directly
    if (connection_pool != NULL) {
        return 0;
    }
    
    // Create connection memory pool
    connection_pool = create_memory_pool(pool_size);
    if (connection_pool == NULL) {
        log_error("Failed to create connection memory pool");
        return -1;
    }
    
    log_info("Connection management module initialized successfully, memory pool initial size: %zu bytes", pool_size);
    return 0;
}

// Clean up connection management module
void cleanup_connection_manager(void) {
    if (connection_pool != NULL) {
        // Get memory pool statistics
        size_t total_size, used_size;
        get_pool_stats(connection_pool, &total_size, &used_size);
        
        log_info("Cleaning up connection memory pool, total size: %zu bytes, used: %zu bytes", total_size, used_size);
        
        // Destroy memory pool
        destroy_memory_pool(connection_pool);
        connection_pool = NULL;
    }
}

// Create connection (unified event loop version)
connection_t *connection_create(int fd, event_loop_t *loop, config_t *config, struct sockaddr_in *client_addr) {
    if (fd < 0 || loop == NULL || config == NULL) {
        return NULL;
    }
    
    connection_t *conn = connection_create_internal(fd, config, client_addr);
    if (conn == NULL) {
        return NULL;
    }
    
    conn->loop = loop;
    
    // Add to event loop
    if (event_loop_add_handler(loop, fd, EVENT_READ, connection_read_callback, connection_write_callback, conn) != 0) {
        log_error("Failed to add connection to event loop");
        connection_destroy(conn);
        return NULL;
    }
    
    log_debug("Created new connection: %s:%d", safe_inet_ntoa(conn->addr.sin_addr), ntohs(conn->addr.sin_port));
    return conn;
}

// Create connection (compatibility function)
connection_t *connection_create_enhanced(int fd, event_loop_t *loop, config_t *config, struct sockaddr_in *client_addr) {
    // Now uniformly use the standard connection_create function
    return connection_create(fd, loop, config, client_addr);
}

// Internal connection creation function
static connection_t *connection_create_internal(int fd, config_t *config, struct sockaddr_in *client_addr) {
    
    // Ensure connection memory pool is initialized
    if (connection_pool == NULL) {
        if (init_connection_manager(CONNECTION_POOL_SIZE) != 0) {
            log_error("Connection memory pool not initialized");
            return NULL;
        }
    }
    
    // Allocate connection structure from memory pool
    connection_t *conn = (connection_t *)pool_malloc(connection_pool, sizeof(connection_t));
    if (conn == NULL) {
        log_error("Unable to allocate connection memory from memory pool");
        return NULL;
    }
    
    // Initialize connection
    memset(conn, 0, sizeof(connection_t));
    conn->fd = fd;
    conn->config = config;
    conn->last_activity = time(NULL);
    conn->timeout = 30;  // Default 30 second timeout
    
    // Allocate read buffer from memory pool
    conn->read_buffer = (char *)pool_malloc(connection_pool, BUFFER_SIZE);
    if (conn->read_buffer == NULL) {
        log_error("Unable to allocate read buffer memory from memory pool");
        pool_free(connection_pool, conn);
        return NULL;
    }
    conn->read_size = BUFFER_SIZE;
    conn->read_pos = 0;
    
    // Allocate write buffer from memory pool
    conn->write_buffer = (char *)pool_malloc(connection_pool, BUFFER_SIZE);
    if (conn->write_buffer == NULL) {
        log_error("Unable to allocate write buffer memory from memory pool");
        pool_free(connection_pool, conn->read_buffer);
        pool_free(connection_pool, conn);
        return NULL;
    }
    conn->write_size = BUFFER_SIZE;
    conn->write_pos = 0;
    
    // Set client address
    if (client_addr != NULL) {
        conn->addr = *client_addr;
    } else {
        // If no address provided, use getpeername to get it
        socklen_t addr_len = sizeof(conn->addr);
        getpeername(fd, (struct sockaddr *)&conn->addr, &addr_len);
    }
    
    log_debug("Created new connection object: %s:%d", safe_inet_ntoa(conn->addr.sin_addr), ntohs(conn->addr.sin_port));
    return conn;
}

// Destroy connection
void connection_destroy(connection_t *conn) {
    // Check if need to return to connection pool
    connection_pool_t *pool = get_worker_connection_pool();
    
    if (pool && conn->keep_alive) {
        // Try to return to connection pool
        connection_pool_return_connection(pool, conn);
        return;
    }
    
    // Destroy connection directly
    connection_destroy_internal(conn);
}

// Internal connection destruction function
static void connection_destroy_internal(connection_t *conn) {
    if (conn == NULL) {
        return;
    }
    
    // Release connection limit count
    if (conn->fd >= 0) {
        char client_ip[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, &conn->addr.sin_addr, client_ip, INET_ADDRSTRLEN) == NULL) {
            strcpy(client_ip, "0.0.0.0");
        }
        release_connection(client_ip);
    }
    
    // Remove from event loop
    if (conn->fd >= 0 && conn->loop != NULL) {
        event_loop_del_handler(conn->loop, conn->fd);
    }
    
    // Close connection
    if (conn->fd >= 0) {
        close(conn->fd);
        conn->fd = -1;
    }
    
    // Decrement active connection count
    extern worker_context_t *get_worker_context(void);
    worker_context_t *worker_ctx = get_worker_context();
    if (worker_ctx != NULL && worker_ctx->active_connections > 0) {
        worker_ctx->active_connections--;
    }
    
    // Free HTTP request resources
    free_http_request(&conn->request);
    
    // Free buffer resources
    if (conn->read_buffer != NULL) {
        pool_free(connection_pool, conn->read_buffer);
        conn->read_buffer = NULL;
    }
    if (conn->write_buffer != NULL) {
        pool_free(connection_pool, conn->write_buffer);
        conn->write_buffer = NULL;
    }
    
    // Free connection object
    pool_free(connection_pool, conn);
}

// Expand buffer
static int expand_buffer(char **buffer, size_t *size, size_t new_size) {
    // Use memory pool to allocate new buffer
    char *new_buffer = (char *)pool_malloc(connection_pool, new_size);
    if (new_buffer == NULL) {
        log_error("Failed to expand buffer from memory pool");
        return -1;
    }
    
    // Copy old buffer content to new buffer
    if (*buffer != NULL && *size > 0) {
        memcpy(new_buffer, *buffer, *size);
        
        // Free old buffer
        pool_free(connection_pool, *buffer);
    }
    
    *buffer = new_buffer;
    *size = new_size;
    return 0;
}

// Read data
static int connection_read(connection_t *conn) {
    if (conn == NULL || conn->fd < 0 || conn->read_buffer == NULL) {
        return -1;
    }
    
    // Ensure buffer has enough space
    if (conn->read_pos >= conn->read_size - 1024) {  // Leave 1KB safety space
        size_t new_size = conn->read_size * 2;
        // Set buffer size limit to prevent malicious clients from sending large amounts of data causing memory exhaustion
        if (new_size > 1024 * 1024 * 10) {  // Maximum 10MB
            log_error("Request data too large, may be malicious request");
            return -2;
        }
        
        if (expand_buffer(&conn->read_buffer, &conn->read_size, new_size) != 0) {
            log_error("Failed to expand read buffer, insufficient memory");
            return -2;
        }
    }
    
    // Read data
    ssize_t n = read(conn->fd, conn->read_buffer + conn->read_pos, conn->read_size - conn->read_pos - 1);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;  // No data to read
        }
        log_error("Failed to read data: %s", strerror(errno));
        if (errno == ECONNRESET || errno == EPIPE) {
            return -1;  // Connection closed, but this is expected error
        }
        return -2;  // Other errors
    } else if (n == 0) {
        log_debug("Client closed connection");
        return -1;  // Connection closed
    }
    
    // Update read position
    conn->read_pos += n;
    conn->read_buffer[conn->read_pos] = '\0';  // Ensure string ends with NULL
    
    return n;
}

// Write data
static int connection_write(connection_t *conn) {
    if (conn == NULL || conn->fd < 0 || conn->write_buffer == NULL || conn->write_pos == 0) {
        return 0;
    }
    
    // Write data
    ssize_t n = write(conn->fd, conn->write_buffer, conn->write_pos);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // Buffer full, need to wait for writable event
            if (event_loop_mod_handler(conn->loop, conn->fd, EVENT_READ | EVENT_WRITE, 
                                      connection_read_callback, connection_write_callback, conn) != 0) {
                log_error("Failed to modify event handler: %s", strerror(errno));
                return -2;
            }
            return 0;  // Buffer full, try again later
        }
        log_error("Failed to write data: %s", strerror(errno));
        if (errno == ECONNRESET || errno == EPIPE) {
            return -1;  // Connection closed, but this is expected error
        }
        return -2;  // Other errors
    }
    
    // Update write position
    if ((size_t)n < conn->write_pos) {
        // Move remaining data to beginning of buffer
        memmove(conn->write_buffer, conn->write_buffer + n, conn->write_pos - n);
        conn->write_pos -= n;
        
        // Ensure continue listening for write events
        if (event_loop_mod_handler(conn->loop, conn->fd, EVENT_READ | EVENT_WRITE, 
                                  connection_read_callback, connection_write_callback, conn) != 0) {
            log_error("Failed to modify event handler: %s", strerror(errno));
            return -2;
        }
    } else {
        conn->write_pos = 0;
        
        // If no more data to write, cancel write event listening
        if (event_loop_mod_handler(conn->loop, conn->fd, EVENT_READ, 
                                  connection_read_callback, NULL, conn) != 0) {
            log_error("Failed to modify event handler: %s", strerror(errno));
            return -2;
        }
    }
    
    return n;
}

// Handle HTTP request
static int handle_request(connection_t *conn) {
    if (conn == NULL || conn->fd < 0 || conn->read_buffer == NULL) {
        log_error("handle_request: invalid parameters");
        log_access("-", "-", "-", 500, 0, "-");
        return -1;
    }
    
    int status_code = 0;
    size_t response_size = 0;
    
    int parse_result = parse_http_request_from_buffer(conn->read_buffer, conn->read_pos, &conn->request);
    if (parse_result == -2) {
        return 1; // Need more data
    } else if (parse_result != 0) {
        status_code = 400;
        send_http_error(conn->fd, status_code, "Request format error", "UTF-8");
        conn->keep_alive = 0;
        log_access(safe_inet_ntoa(conn->addr.sin_addr), "-", "-", status_code, 0, "-");
        return -1;
    }
    
    // Check if request method is supported
    if (conn->request.method != HTTP_GET && conn->request.method != HTTP_POST && 
        conn->request.method != HTTP_HEAD && conn->request.method != HTTP_OPTIONS) {
        log_warn("Unsupported HTTP method: %s", http_method_str(conn->request.method));
        send_http_error(conn->fd, 405, "Method not allowed", "UTF-8");
        conn->keep_alive = 0;
        return -1;
    }
    
    // Find matching route
    route_t *route = find_route(conn->config, conn->request.path);
    
    if (route == NULL) {
        status_code = 404;
        log_warn("No matching route found: %s", conn->request.path);
        send_http_error(conn->fd, status_code, "Not found", "UTF-8");
        conn->keep_alive = 0;
        log_access(safe_inet_ntoa(conn->addr.sin_addr), http_method_str(conn->request.method), conn->request.path, status_code, 0, get_header_value(&conn->request, "User-Agent"));
        return -1;
    }
    
    // Validate request
    auth_result_t auth_result;
    if (!validate_request(&conn->request, route, &auth_result)) {
        status_code = 403;
        log_warn("Request validation failed: %s", auth_result.error_message);
        send_http_error(conn->fd, status_code, auth_result.error_message, route->charset);
        conn->keep_alive = 0;
        size_t error_page_size = strlen(auth_result.error_message) + 800;
        log_access(safe_inet_ntoa(conn->addr.sin_addr), http_method_str(conn->request.method), conn->request.path, status_code, error_page_size, get_header_value(&conn->request, "User-Agent"));
        return -1;
    }
    
    // Handle request based on route type
    int handler_result = 0;
    
    switch (route->type) {
        case ROUTE_PROXY:
            handler_result = proxy_request(conn->fd, &conn->request, route, &status_code, &response_size);
            if (handler_result != 0) {
                log_error("Proxy request failed");
                conn->keep_alive = 0;
                log_access(safe_inet_ntoa(conn->addr.sin_addr), http_method_str(conn->request.method), conn->request.path, status_code ? status_code : 502, response_size, get_header_value(&conn->request, "User-Agent"));
                return -1;
            }
            // If proxy succeeds but status_code is 0, set to 200
            if (status_code == 0) {
                status_code = 200;
            }
            break;
            
        case ROUTE_STATIC:
            handler_result = handle_local_file(conn->fd, &conn->request, route, &status_code, &response_size);
            if (handler_result != 0) {
                log_error("Failed to handle static files request");
                send_http_error(conn->fd, status_code ? status_code : 500, "Failed to handle static files request", route->charset);
                conn->keep_alive = 0;
                log_access(safe_inet_ntoa(conn->addr.sin_addr), http_method_str(conn->request.method), conn->request.path, status_code ? status_code : 500, 0, get_header_value(&conn->request, "User-Agent"));
                return -1;
            }
            // If static files handling succeeds but status_code is 0, set to 200
            if (status_code == 0) {
                status_code = 200;
            }
            break;
            
        default:
            log_error("Unknown route type: %d", route->type);
            send_http_error(conn->fd, 500, "Internal server error", route->charset);
            conn->keep_alive = 0;
            log_access(safe_inet_ntoa(conn->addr.sin_addr), http_method_str(conn->request.method), conn->request.path, 500, 0, get_header_value(&conn->request, "User-Agent"));
            return -1;
    }
    
    // Record access log (normal response)
    char *user_agent = get_header_value(&conn->request, "User-Agent");
    // log_info("Preparing to record access log: IP=%s, method=%s, path=%s, status_code=%d, response_size=%zu", 
    //          safe_inet_ntoa(conn->addr.sin_addr), 
    //          http_method_str(conn->request.method), 
    //          conn->request.path, 
    //          status_code, 
    //          response_size);
    
    log_access(safe_inet_ntoa(conn->addr.sin_addr), 
              http_method_str(conn->request.method), 
              conn->request.path, 
              status_code, 
              response_size, 
              user_agent);
    
    // Force short connection, connection should be closed immediately after request processing
    conn->keep_alive = 0;
    return 0;
}

// Check if connection has timed out
static int connection_is_timeout(connection_t *conn) {
    if (conn == NULL) {
        return 0;
    }
    
    time_t now = time(NULL);
    
    // For keep-alive connections, use configured timeout
    if (conn->keep_alive) {
        return (now - conn->last_activity) > conn->timeout;
    }
    
    // For non-keep-alive connections, use shorter timeout (5 seconds)
    return (now - conn->last_activity) > 5;
}

// Read callback function
void connection_read_callback(int fd, void *arg) {
    connection_t *conn = (connection_t *)arg;
    if (conn == NULL) {
        log_error("connection_read_callback: conn is NULL, fd=%d", fd);
        return;
    }
    
    if (connection_is_timeout(conn)) {
        log_debug("Connection timed out, closing connection fd=%d", conn->fd);
        connection_destroy(conn);
        return;
    }
    
    conn->last_activity = time(NULL);
    int read_result = connection_read(conn);
    
    if (read_result < 0) {
        if (read_result == -1) {
            log_debug("Connection normally closed fd=%d", conn->fd);
        } else {
            log_error("Connection read error fd=%d", conn->fd);
        }
        connection_destroy(conn);
        return;
    }
    
    if (conn->read_pos > 0) {
        int handle_result = handle_request(conn);
        
        if (handle_result == 0) {
            // Remove processed data
            char *body_start = strstr(conn->read_buffer, "\r\n\r\n");
            if (body_start != NULL) {
                size_t header_length = (body_start - conn->read_buffer) + 4;
                
                // If there's Content-Length header, also add body length
                char *content_length_str = get_header_value(&conn->request, "Content-Length");
                if (content_length_str != NULL) {
                    size_t content_length = atoi(content_length_str);
                    header_length += content_length;
                }
                
                // Ensure no out-of-bounds
                if (header_length <= conn->read_pos) {
                    if (header_length < conn->read_pos) {
                        memmove(conn->read_buffer, conn->read_buffer + header_length, conn->read_pos - header_length);
                        conn->read_pos -= header_length;
                    } else {
                        conn->read_pos = 0;
                    }
                }
            } else {
                conn->read_pos = 0;
            }
            
            // Free request resources
            free_http_request(&conn->request);
            memset(&conn->request, 0, sizeof(http_request_t));
            
            // For short connections, close connection immediately
            if (!conn->keep_alive) {
                connection_destroy(conn);
                return;
            }
            
            // If there's still data in buffer, continue processing next request
            if (conn->read_pos > 0) {
                connection_read_callback(fd, arg);
                return;
            }
        } else if (handle_result == -1) {
            connection_destroy(conn);
            return;
        }
    }
}

// Write callback function
void connection_write_callback(int fd, void *arg) {
    (void)fd; // avoid unused parameter warning
    connection_t *conn = (connection_t *)arg;
    
    if (conn == NULL || conn->fd < 0) {
        log_error("Invalid connection object");
        return;
    }
    
    // Check if connection has timed out
    if (connection_is_timeout(conn)) {
        log_debug("Connection timed out, closing connection");
        connection_destroy(conn);
        return;
    }
    
    // Update last activity time
    conn->last_activity = time(NULL);
    
    // Write data
    int write_result = connection_write(conn);
    if (write_result < 0) {
        log_error("Failed to write data, closing connection");
        connection_destroy(conn);
        return;
    }
}

// Accept new connection callback function
void accept_connection_callback(int server_fd, void *arg) {
    struct {
        event_loop_t *loop;
        config_t *config;
    } *ctx = (void *)arg;
    
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
    
    if (client_fd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }
        log_error("Accept connection failed: %s", strerror(errno));
        return;
    }
    
    // Get client IP address
    char client_ip[INET_ADDRSTRLEN];
    if (inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN) == NULL) {
        strcpy(client_ip, "0.0.0.0");
    }
    
    // Check connection limit
    if (check_connection_limit(client_ip) != 0) {
        log_warn("Rejecting connection %s: exceeded connection limit", client_ip);
        close(client_fd);
        return;
    }
    
    // Check request rate limit
    if (check_rate_limit(client_ip) != 0) {
        log_warn("Rejecting connection %s: exceeded request rate limit", client_ip);
        release_connection(client_ip);
        close(client_fd);
        return;
    }
    
    // Set non-blocking mode
    int flags = fcntl(client_fd, F_GETFL, 0);
    if (flags < 0 || fcntl(client_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        log_error("Failed to set non-blocking mode: %s", strerror(errno));
        close(client_fd);
        return;
    }
    
    // Create new connection
    connection_t *conn = connection_create(client_fd, ctx->loop, ctx->config, &client_addr);
    if (conn == NULL) {
        log_error("Failed to create connection");
        close(client_fd);
        return;
    }
    
    log_debug("Accept new connection: %s:%d", client_ip, ntohs(client_addr.sin_port));
}


/**
 * Compress connection memory pool, free unused memory blocks
 */
int compress_connection_pool(void) {
    if (connection_pool == NULL) {
        return 0;
    }
    
    // Get statistics before compression
    size_t total_before, used_before;
    get_pool_stats(connection_pool, &total_before, &used_before);
    
    // Execute memory pool compression
    int freed_blocks = compress_memory_pool(connection_pool);
    
    // Get statistics after compression
    size_t total_after, used_after;
    get_pool_stats(connection_pool, &total_after, &used_after);
    
    if (freed_blocks > 0) {
        log_debug("Connection memory pool compression completed: freed %d blocks, total memory %zu -> %zu bytes", 
                 freed_blocks, total_before, total_after);
    }
    
    return freed_blocks;
}

/**
 * X-Server High-Performance Logging System - TLS Optimized Version
 * Uses thread-local buffers + batch writing hybrid architecture
 * Minimizes lock contention while ensuring data integrity
 */

#include "../include/logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/time.h>
#include <errno.h>
#include <sys/stat.h>
#include <pthread.h>

// TLS buffer configuration
#define TLS_BUFFER_SIZE 8192        // 8KB thread-local buffer
#define BATCH_FLUSH_THRESHOLD 6144  // Trigger batch flush at 6KB
#define MAX_LOG_ENTRY_SIZE 1024     // Maximum single log entry size
#define IDLE_FLUSH_INTERVAL 5       // Force flush after 5 seconds idle
#define PERIODIC_FLUSH_INTERVAL 30  // Periodic force flush every 30 seconds

// Thread-local buffer structure
typedef struct {
    char buffer[TLS_BUFFER_SIZE];
    size_t write_pos;
    size_t flush_count;
    pthread_t thread_id;
    int initialized;
    time_t last_write_time;    // Last write time
    time_t last_flush_time;    // Last flush time
} tls_log_buffer_t;

// Global shared buffer (for batch writing)
typedef struct {
    char server_buffer[LOGGER_BUFFER_SIZE];
    char access_buffer[LOGGER_BUFFER_SIZE];
    size_t server_pos;
    size_t access_pos;
    time_t server_last_write_time;    // Server log last write time
    time_t access_last_write_time;    // Access log last write time
    time_t server_last_flush_time;    // Server log last flush time
    time_t access_last_flush_time;    // Access log last flush time
    pthread_mutex_t server_mutex;
    pthread_mutex_t access_mutex;
} global_log_buffer_t;

// Extended performance statistics structure
typedef struct {
    volatile uint64_t total_logs;
    volatile uint64_t total_bytes;
    volatile uint64_t flush_count;
    volatile uint64_t tls_flush_count;  // TLS buffer flush count
    volatile uint64_t drop_count;
    volatile uint64_t error_count;
} extended_logger_stats_t;

// Global state
static volatile int g_initialized = 0;
static logger_config_t g_config;
static FILE *g_server_log = NULL;
static FILE *g_access_log = NULL;
static pthread_mutex_t g_init_mutex = PTHREAD_MUTEX_INITIALIZER;

// Global shared buffer
static global_log_buffer_t g_global_buffer = {
    .server_pos = 0,
    .access_pos = 0,
    .server_mutex = PTHREAD_MUTEX_INITIALIZER,
    .access_mutex = PTHREAD_MUTEX_INITIALIZER
};

// Performance statistics
static extended_logger_stats_t g_stats = {0};

// Thread-local storage
static __thread tls_log_buffer_t *g_tls_buffer = NULL;
static __thread time_t g_cached_time = 0;
static __thread char g_cached_time_str[32] = {0};

// Atomic operation helper function
static inline void atomic_add_uint64(volatile uint64_t *ptr, uint64_t value) {
    __sync_fetch_and_add(ptr, value);
}

// Get formatted time string (thread-local cache)
static const char *get_time_string(void) {
    time_t now = time(NULL);
    if (now != g_cached_time) {
        struct tm *tm_info = localtime(&now);
        strftime(g_cached_time_str, sizeof(g_cached_time_str), 
                "%Y-%m-%d %H:%M:%S", tm_info);
        g_cached_time = now;
    }
    return g_cached_time_str;
}

// Get high-precision time string (including microseconds)
static void get_precise_time_string(char *buffer, size_t size) {
    struct timeval tv;
    struct tm *tm_info;
    
    gettimeofday(&tv, NULL);
    tm_info = localtime(&tv.tv_sec);
    
    snprintf(buffer, size, "%04d-%02d-%02d %02d:%02d:%02d.%06d",
             tm_info->tm_year + 1900,
             tm_info->tm_mon + 1,
             tm_info->tm_mday,
             tm_info->tm_hour,
             tm_info->tm_min,
             tm_info->tm_sec,
             (int)tv.tv_usec);
}

// Initialize thread-local buffer
static int init_tls_buffer(void) {
    if (g_tls_buffer && g_tls_buffer->initialized) {
        return 0;
    }
    
    if (!g_tls_buffer) {
        g_tls_buffer = malloc(sizeof(tls_log_buffer_t));
        if (!g_tls_buffer) {
            return -1;
        }
    }
    
    time_t now = time(NULL);
    memset(g_tls_buffer, 0, sizeof(tls_log_buffer_t));
    g_tls_buffer->write_pos = 0;
    g_tls_buffer->flush_count = 0;
    g_tls_buffer->thread_id = pthread_self();
    g_tls_buffer->initialized = 1;
    g_tls_buffer->last_write_time = now;
    g_tls_buffer->last_flush_time = now;
    
    return 0;
}

// Check if buffer needs to be flushed
static int should_flush_buffer(int force_flush) {
    if (!g_tls_buffer || g_tls_buffer->write_pos == 0) {
        return 0;
    }
    
    // Force flush
    if (force_flush) {
        return 1;
    }
    
    // Buffer reached threshold
    if (g_tls_buffer->write_pos >= BATCH_FLUSH_THRESHOLD) {
        return 1;
    }
    
    time_t now = time(NULL);
    
    // Idle time exceeded threshold (5 seconds without new log writes)
    if (now - g_tls_buffer->last_write_time >= IDLE_FLUSH_INTERVAL) {
        return 1;
    }
    
    // Periodic flush (force flush every 30 seconds)
    if (now - g_tls_buffer->last_flush_time >= PERIODIC_FLUSH_INTERVAL) {
        return 1;
    }
    
    return 0;
}

// Batch flush TLS buffer to global buffer
static void flush_tls_to_global(int is_server_log, int force_flush) {
    if (!should_flush_buffer(force_flush)) {
        return;
    }
    
    pthread_mutex_t *mutex;
    char *global_buf;
    size_t *global_pos;
    FILE *log_file;
    
    if (is_server_log) {
        mutex = &g_global_buffer.server_mutex;
        global_buf = g_global_buffer.server_buffer;
        global_pos = &g_global_buffer.server_pos;
        log_file = g_server_log;
    } else {
        mutex = &g_global_buffer.access_mutex;
        global_buf = g_global_buffer.access_buffer;
        global_pos = &g_global_buffer.access_pos;
        log_file = g_access_log;
    }
    
    // Lock for batch writing
    pthread_mutex_lock(mutex);
    
    // Check global buffer space
    if (*global_pos + g_tls_buffer->write_pos >= LOGGER_BUFFER_SIZE) {
        // Global buffer full, write directly to file
        if (log_file && *global_pos > 0) {
            size_t written = fwrite(global_buf, 1, *global_pos, log_file);
            if (written == *global_pos) {
                fflush(log_file);
                *global_pos = 0;
                atomic_add_uint64(&g_stats.flush_count, 1);
            } else {
                atomic_add_uint64(&g_stats.error_count, 1);
            }
        }
    }
    
    // Copy TLS buffer content to global buffer
    if (*global_pos + g_tls_buffer->write_pos < LOGGER_BUFFER_SIZE) {
        memcpy(global_buf + *global_pos, g_tls_buffer->buffer, g_tls_buffer->write_pos);
        *global_pos += g_tls_buffer->write_pos;
        atomic_add_uint64(&g_stats.total_bytes, g_tls_buffer->write_pos);
        
        // Update global buffer last write time
        time_t now = time(NULL);
        if (is_server_log) {
            g_global_buffer.server_last_write_time = now;
        } else {
            g_global_buffer.access_last_write_time = now;
        }
    } else {
        // Write directly to file
        if (log_file) {
            size_t written = fwrite(g_tls_buffer->buffer, 1, g_tls_buffer->write_pos, log_file);
            if (written == g_tls_buffer->write_pos) {
                fflush(log_file);
                atomic_add_uint64(&g_stats.total_bytes, written);
                atomic_add_uint64(&g_stats.flush_count, 1);
                
                // Update global buffer last flush time
                time_t now = time(NULL);
                if (is_server_log) {
                    g_global_buffer.server_last_flush_time = now;
                } else {
                    g_global_buffer.access_last_flush_time = now;
                }
            } else {
                atomic_add_uint64(&g_stats.error_count, 1);
            }
        } else {
            atomic_add_uint64(&g_stats.drop_count, 1);
        }
    }
    
    pthread_mutex_unlock(mutex);
    
    // Reset TLS buffer and update timestamp
    g_tls_buffer->write_pos = 0;
    g_tls_buffer->flush_count++;
    g_tls_buffer->last_flush_time = time(NULL);
    atomic_add_uint64(&g_stats.tls_flush_count, 1);
}

// Write to TLS buffer
static void write_to_tls_buffer(const char *data, size_t len, int is_server_log) {
    if (!g_initialized) {
        return;
    }
    
    // Initialize TLS buffer
    if (init_tls_buffer() != 0) {
        atomic_add_uint64(&g_stats.drop_count, 1);
        return;
    }
    
    // Check single log entry size
    if (len > MAX_LOG_ENTRY_SIZE) {
        atomic_add_uint64(&g_stats.drop_count, 1);
        return;
    }
    
    // Check TLS buffer space
    if (g_tls_buffer->write_pos + len >= TLS_BUFFER_SIZE) {
        // TLS buffer full, force flush to corresponding global buffer
        flush_tls_to_global(is_server_log, 1);
    }
    
    // Write to TLS buffer
    if (g_tls_buffer->write_pos + len < TLS_BUFFER_SIZE) {
        memcpy(g_tls_buffer->buffer + g_tls_buffer->write_pos, data, len);
        g_tls_buffer->write_pos += len;
        g_tls_buffer->last_write_time = time(NULL);  // Update last write time
        atomic_add_uint64(&g_stats.total_logs, 1);
        
        // Check if flush is needed (including idle and periodic checks)
        flush_tls_to_global(is_server_log, 0);
    } else {
        atomic_add_uint64(&g_stats.drop_count, 1);
    }
}

// Get log filename
static void get_log_filename(char *filename, size_t size, const char *prefix) {
    if (g_config.daily_rotation) {
        time_t now = time(NULL);
        struct tm *tm_info = localtime(&now);
        snprintf(filename, size, "%s/%s.%04d-%02d-%02d.log",
                g_config.log_dir, prefix,
                tm_info->tm_year + 1900,
                tm_info->tm_mon + 1,
                tm_info->tm_mday);
    } else {
        snprintf(filename, size, "%s/%s.log", g_config.log_dir, prefix);
    }
}

// Create log directory
static int create_log_directory(const char *path) {
    struct stat st = {0};
    if (stat(path, &st) == -1) {
        if (mkdir(path, 0755) == -1) {
            return -1;
        }
    }
    return 0;
}

// Initialize logging system
int init_logger(const char *log_path, int level, int daily_rotation) {
    if (g_initialized) {
        return 0;
    }
    
    pthread_mutex_lock(&g_init_mutex);
    
    // Double-checked locking
    if (g_initialized) {
        pthread_mutex_unlock(&g_init_mutex);
        return 0;
    }
    
    // Set configuration
    strncpy(g_config.log_dir, log_path ? log_path : "./logs", 
            sizeof(g_config.log_dir) - 1);
    g_config.level = level;
    g_config.daily_rotation = daily_rotation;
    g_config.buffer_size = LOGGER_BUFFER_SIZE;
    
    // Create log directory
    if (create_log_directory(g_config.log_dir) != 0) {
        pthread_mutex_unlock(&g_init_mutex);
        return -1;
    }
    
    // Initialize global buffer
    memset(&g_global_buffer, 0, sizeof(global_log_buffer_t));
    time_t now = time(NULL);
    g_global_buffer.server_pos = 0;
    g_global_buffer.access_pos = 0;
    g_global_buffer.server_last_write_time = now;
    g_global_buffer.access_last_write_time = now;
    g_global_buffer.server_last_flush_time = now;
    g_global_buffer.access_last_flush_time = now;
    
    // Open log files
    char server_filename[512], access_filename[512];
    get_log_filename(server_filename, sizeof(server_filename), "server");
    get_log_filename(access_filename, sizeof(access_filename), "access");
    
    g_server_log = fopen(server_filename, "a");
    g_access_log = fopen(access_filename, "a");
    
    if (!g_server_log || !g_access_log) {
        if (g_server_log) fclose(g_server_log);
        if (g_access_log) fclose(g_access_log);
        pthread_mutex_unlock(&g_init_mutex);
        return -1;
    }
    
    // Set file permissions
    chmod(server_filename, 0640);
    chmod(access_filename, 0640);
    
    // Reset statistics
    memset((void*)&g_stats, 0, sizeof(g_stats));
    
    g_initialized = 1;
    
    pthread_mutex_unlock(&g_init_mutex);
    
    // Log initialization success
    if (g_config.level <= LOG_LEVEL_INFO) {
        log_info("TLS optimized logging system initialized successfully, directory: %s, level: %d, TLS buffer: %dKB", 
                g_config.log_dir, g_config.level, TLS_BUFFER_SIZE/1024);
    }
    
    return 0;
}

// Update logger configuration
int update_logger_config(const char *log_path, int level, int daily_rotation) {
    if (!g_initialized) {
        // In multi-process environment, if logging system is not initialized, it might be because this is a Worker process
        // and the logging system has already been initialized in the Master process
        // In this case, we should avoid duplicate initialization and silently return success
        return 0;  // Silent return to avoid duplicate initialization
    }
    
    pthread_mutex_lock(&g_init_mutex);
    
    // Force flush all buffers
    logger_flush();
    
    // Update configuration
    if (log_path) {
        strncpy(g_config.log_dir, log_path, sizeof(g_config.log_dir) - 1);
    }
    g_config.level = level;
    g_config.daily_rotation = daily_rotation;
    
    pthread_mutex_unlock(&g_init_mutex);
    
    // Only output configuration update log in Master process
    if (getenv("WORKER_PROCESS_ID") == NULL && g_config.level <= LOG_LEVEL_INFO) {
        log_info("Logger configuration updated, directory: %s, level: %d", g_config.log_dir, g_config.level);
    }
    
    return 0;
}

// Close logging system
void close_logger(void) {
    if (!g_initialized) {
        return;
    }
    
    pthread_mutex_lock(&g_init_mutex);
    
    if (!g_initialized) {
        pthread_mutex_unlock(&g_init_mutex);
        return;
    }
    
    // Force flush all buffers
    logger_flush();
    
    // Flush global buffers to files
    pthread_mutex_lock(&g_global_buffer.server_mutex);
    if (g_global_buffer.server_pos > 0 && g_server_log) {
        size_t written = fwrite(g_global_buffer.server_buffer, 1, g_global_buffer.server_pos, g_server_log);
        if (written == g_global_buffer.server_pos) {
            fflush(g_server_log);
        } else {
            atomic_add_uint64(&g_stats.error_count, 1);
        }
        g_global_buffer.server_pos = 0;
    }
    pthread_mutex_unlock(&g_global_buffer.server_mutex);
    
    pthread_mutex_lock(&g_global_buffer.access_mutex);
    if (g_global_buffer.access_pos > 0 && g_access_log) {
        size_t written = fwrite(g_global_buffer.access_buffer, 1, g_global_buffer.access_pos, g_access_log);
        if (written == g_global_buffer.access_pos) {
            fflush(g_access_log);
        } else {
            atomic_add_uint64(&g_stats.error_count, 1);
        }
        g_global_buffer.access_pos = 0;
    }
    pthread_mutex_unlock(&g_global_buffer.access_mutex);
    
    // Close files
    if (g_server_log) {
        fclose(g_server_log);
        g_server_log = NULL;
    }
    if (g_access_log) {
        fclose(g_access_log);
        g_access_log = NULL;
    }
    
    g_initialized = 0;
    
    pthread_mutex_unlock(&g_init_mutex);
}

// Generic log message function
static void log_message(log_level_t level, const char *format, va_list args) {
    if (!g_initialized || level < g_config.level) {
        return;
    }
    
    const char *level_str[] = {"DEBUG", "INFO", "WARN", "ERROR"};
    char log_line[MAX_LOG_ENTRY_SIZE];
    int len;
    
    // Format log message
    len = snprintf(log_line, sizeof(log_line), "[%s] [%s] ", 
                   get_time_string(), level_str[level]);
    if (len > 0 && len < MAX_LOG_ENTRY_SIZE - 1) {
        len += vsnprintf(log_line + len, sizeof(log_line) - len - 1, format, args);
        if (len > 0 && len < MAX_LOG_ENTRY_SIZE - 1) {
            log_line[len++] = '\n';
            log_line[len] = '\0';
            
            // Write to TLS buffer
            write_to_tls_buffer(log_line, len, 1);  // 1 indicates server log
        }
    }
}

// API compatibility functions
void log_debug(const char *format, ...) {
    va_list args;
    va_start(args, format);
    log_message(LOG_LEVEL_DEBUG, format, args);
    va_end(args);
}

void log_info(const char *format, ...) {
    va_list args;
    va_start(args, format);
    log_message(LOG_LEVEL_INFO, format, args);
    va_end(args);
}

void log_warn(const char *format, ...) {
    va_list args;
    va_start(args, format);
    log_message(LOG_LEVEL_WARN, format, args);
    va_end(args);
}

void log_error(const char *format, ...) {
    va_list args;
    va_start(args, format);
    log_message(LOG_LEVEL_ERROR, format, args);
    va_end(args);
}

// Access log - write directly to global buffer
void log_access(const char *client_ip, const char *method, const char *path,
               int status_code, size_t response_size, const char *user_agent) {
    if (!g_initialized) {
        return;
    }
    
    char precise_time[64];
    get_precise_time_string(precise_time, sizeof(precise_time));
    
    char log_line[MAX_LOG_ENTRY_SIZE];
    int len = snprintf(log_line, sizeof(log_line),
                      "%s - - [%s] \"%s %s HTTP/1.1\" %d %zu \"-\" \"%s\"\n",
                      client_ip ? client_ip : "-",
                      precise_time,
                      method ? method : "-",
                      path ? path : "-",
                      status_code,
                      response_size,
                      user_agent ? user_agent : "-");
    
    if (len > 0 && len < MAX_LOG_ENTRY_SIZE) {
        // Write directly to access log global buffer to avoid TLS buffer confusion
        pthread_mutex_lock(&g_global_buffer.access_mutex);
        
        // Check global buffer space
        if (g_global_buffer.access_pos + len >= LOGGER_BUFFER_SIZE) {
            // Global buffer full, write directly to file
            if (g_access_log && g_global_buffer.access_pos > 0) {
                size_t written = fwrite(g_global_buffer.access_buffer, 1, g_global_buffer.access_pos, g_access_log);
                if (written == g_global_buffer.access_pos) {
                    fflush(g_access_log);
                    atomic_add_uint64(&g_stats.flush_count, 1);
                    g_global_buffer.access_last_flush_time = time(NULL);
                } else {
                    atomic_add_uint64(&g_stats.error_count, 1);
                }
                g_global_buffer.access_pos = 0;
            }
        }
        
        // Write to global buffer
        if (g_global_buffer.access_pos + len < LOGGER_BUFFER_SIZE) {
            memcpy(g_global_buffer.access_buffer + g_global_buffer.access_pos, log_line, len);
            g_global_buffer.access_pos += len;
            g_global_buffer.access_last_write_time = time(NULL);
            atomic_add_uint64(&g_stats.total_logs, 1);
            atomic_add_uint64(&g_stats.total_bytes, len);
        } else {
            // Write directly to file
            if (g_access_log) {
                size_t written = fwrite(log_line, 1, (size_t)len, g_access_log);
                if (written == (size_t)len) {
                    fflush(g_access_log);
                    atomic_add_uint64(&g_stats.total_logs, 1);
                    atomic_add_uint64(&g_stats.total_bytes, len);
                    atomic_add_uint64(&g_stats.flush_count, 1);
                    g_global_buffer.access_last_flush_time = time(NULL);
                } else {
                    atomic_add_uint64(&g_stats.error_count, 1);
                }
            } else {
                atomic_add_uint64(&g_stats.drop_count, 1);
            }
        }
        
        pthread_mutex_unlock(&g_global_buffer.access_mutex);
    }
}

// Force flush buffers
void logger_flush(void) {
    if (!g_initialized) {
        return;
    }
    
    // Flush current thread's TLS buffer
    if (g_tls_buffer && g_tls_buffer->write_pos > 0) {
        flush_tls_to_global(1, 1);  // Force flush server log
        flush_tls_to_global(0, 1);  // Force flush access log
    }
}

// Force flush global buffers to files
static void flush_global_buffers_to_file(void) {
    time_t now = time(NULL);
    
    // Flush server log global buffer
    pthread_mutex_lock(&g_global_buffer.server_mutex);
    if (g_global_buffer.server_pos > 0 && g_server_log) {
        // Check if flush is needed (idle 5 seconds or periodic 30 seconds)
        if ((now - g_global_buffer.server_last_write_time >= IDLE_FLUSH_INTERVAL) ||
            (now - g_global_buffer.server_last_flush_time >= PERIODIC_FLUSH_INTERVAL)) {
            
            size_t written = fwrite(g_global_buffer.server_buffer, 1, g_global_buffer.server_pos, g_server_log);
            if (written == g_global_buffer.server_pos) {
                fflush(g_server_log);
                atomic_add_uint64(&g_stats.flush_count, 1);
                g_global_buffer.server_last_flush_time = now;
            } else {
                atomic_add_uint64(&g_stats.error_count, 1);
            }
            g_global_buffer.server_pos = 0;
        }
    }
    pthread_mutex_unlock(&g_global_buffer.server_mutex);
    
    // Flush access log global buffer
    pthread_mutex_lock(&g_global_buffer.access_mutex);
    if (g_global_buffer.access_pos > 0 && g_access_log) {
        // Check if flush is needed (idle 5 seconds or periodic 30 seconds)
        if ((now - g_global_buffer.access_last_write_time >= IDLE_FLUSH_INTERVAL) ||
            (now - g_global_buffer.access_last_flush_time >= PERIODIC_FLUSH_INTERVAL)) {
            
            size_t written = fwrite(g_global_buffer.access_buffer, 1, g_global_buffer.access_pos, g_access_log);
            if (written == g_global_buffer.access_pos) {
                fflush(g_access_log);
                atomic_add_uint64(&g_stats.flush_count, 1);
                g_global_buffer.access_last_flush_time = now;
            } else {
                atomic_add_uint64(&g_stats.error_count, 1);
            }
            g_global_buffer.access_pos = 0;
        }
    }
    pthread_mutex_unlock(&g_global_buffer.access_mutex);
}

// Check and flush idle buffers (called by main loop)
void logger_check_idle_flush(void) {
    if (!g_initialized) {
        return;
    }
    
    // 1. Check if current thread's TLS buffer needs idle flush
    if (g_tls_buffer && g_tls_buffer->write_pos > 0) {
        time_t now = time(NULL);
        
        // Check idle time or periodic flush
        if ((now - g_tls_buffer->last_write_time >= IDLE_FLUSH_INTERVAL) ||
            (now - g_tls_buffer->last_flush_time >= PERIODIC_FLUSH_INTERVAL)) {
            // Note: TLS buffer may contain mixed data from server and access logs
            // Since we cannot distinguish, we need to try flushing to both
            flush_tls_to_global(1, 1);  // Flush to server log global buffer
            flush_tls_to_global(0, 1);  // Flush to access log global buffer
        }
    }
    
    // 2. Force flush global buffers to files (this is critical!)
    // Even if TLS buffer is empty, global buffer may have data written by other threads
    flush_global_buffers_to_file();
}

// Get performance statistics
void logger_get_stats(logger_stats_t *stats) {
    if (stats && g_initialized) {
        memcpy(stats, (void*)&g_stats, sizeof(logger_stats_t));
    }
}

// Reset statistics
void logger_reset_stats(void) {
    if (g_initialized) {
        memset((void*)&g_stats, 0, sizeof(logger_stats_t));
    }
}

// Clean up TLS buffer on thread exit
void logger_thread_cleanup(void) {
    if (g_tls_buffer) {
        // Flush remaining data
        if (g_tls_buffer->write_pos > 0) {
            flush_tls_to_global(1, 1);
            flush_tls_to_global(0, 1);
        }
        
        // Free TLS buffer
        free(g_tls_buffer);
        g_tls_buffer = NULL;
    }
}
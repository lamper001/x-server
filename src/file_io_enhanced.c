/**
 * Enhanced File I/O Module Implementation
 * Provides zero-copy, asynchronous I/O, file caching and other high-performance file processing capabilities
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <stdatomic.h>

#if defined(__linux__)
#include <sys/sendfile.h>
#elif defined(__APPLE__)
#include <sys/uio.h>
#endif

#include "../include/file_io_enhanced.h"
#include "../include/logger.h"

// Global variables
static file_cache_manager_t *g_cache_manager = NULL;
static file_io_stats_t g_stats = {0};
static file_io_config_t g_config = {0};
static atomic_int g_initialized = 0;

// Hash function
static size_t hash_string(const char *str) {
    size_t hash = 5381;
    int c;
    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c;
    }
    return hash;
}

// Get current timestamp (nanoseconds)
static uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

// Cache cleanup thread
static void *cache_cleanup_thread(void *arg) {
    file_cache_manager_t *manager = (file_cache_manager_t *)arg;
    time_t now;
    file_cache_item_t *item, *prev, *next;
    
    while (!atomic_load(&manager->stop_cleanup)) {
        pthread_mutex_lock(&manager->mutex);
        
        now = time(NULL);
        
        // Iterate through all buckets
        for (size_t i = 0; i < manager->bucket_count; i++) {
            prev = NULL;
            item = manager->buckets[i];
            
            while (item != NULL) {
                next = item->next;
                
                // Check if expired (not accessed for more than 1 hour)
                if (now - item->access_time > 3600) {
                    // Remove expired item
                    if (prev == NULL) {
                        manager->buckets[i] = next;
                    } else {
                        prev->next = next;
                    }
                    
                    // Free memory
                    free(item->path);
                    free(item->data);
                    manager->current_size -= item->size;
                    free(item);
                } else {
                    prev = item;
                }
                
                item = next;
            }
        }
        
        pthread_mutex_unlock(&manager->mutex);
        
        // Wait for cleanup interval
        sleep(g_config.cache_cleanup_interval);
    }
    
    return NULL;
}

// Initialize file I/O module
int file_io_enhanced_init(const file_io_config_t *config) {
    if (atomic_load(&g_initialized)) {
        return 0; // Already initialized
    }
    
    if (!config) {
        return -1;
    }
    
    // Copy configuration
    memcpy(&g_config, config, sizeof(file_io_config_t));
    
    // Set default values
    if (g_config.cache_size == 0) g_config.cache_size = 100; // 100MB
    if (g_config.max_file_size == 0) g_config.max_file_size = 50; // 50MB
    if (g_config.read_buffer_size == 0) g_config.read_buffer_size = 8192;
    if (g_config.write_buffer_size == 0) g_config.write_buffer_size = 8192;
    if (g_config.cache_cleanup_interval == 0) g_config.cache_cleanup_interval = 300; // 5 minutes
    
    // Initialize cache manager
    g_cache_manager = malloc(sizeof(file_cache_manager_t));
    if (!g_cache_manager) {
        return -1;
    }
    
    g_cache_manager->bucket_count = 1024; // 1024 buckets
    g_cache_manager->max_size = g_config.cache_size * 1024 * 1024; // Convert to bytes
    g_cache_manager->current_size = 0;
    
    g_cache_manager->buckets = calloc(g_cache_manager->bucket_count, sizeof(file_cache_item_t *));
    if (!g_cache_manager->buckets) {
        free(g_cache_manager);
        return -1;
    }
    
    pthread_mutex_init(&g_cache_manager->mutex, NULL);
    atomic_store(&g_cache_manager->stop_cleanup, 0);
    
    // Start cleanup thread
    if (pthread_create(&g_cache_manager->cleanup_thread, NULL, cache_cleanup_thread, g_cache_manager) != 0) {
        pthread_mutex_destroy(&g_cache_manager->mutex);
        free(g_cache_manager->buckets);
        free(g_cache_manager);
        return -1;
    }
    
    // Reset statistics
    memset(&g_stats, 0, sizeof(g_stats));
    
    atomic_store(&g_initialized, 1);
    
    log_info("Enhanced file I/O module initialization completed");
    return 0;
}

// Destroy file I/O module
void file_io_enhanced_destroy(void) {
    if (!atomic_load(&g_initialized)) {
        return;
    }
    
    if (g_cache_manager) {
        // Stop cleanup thread
        atomic_store(&g_cache_manager->stop_cleanup, 1);
        pthread_join(g_cache_manager->cleanup_thread, NULL);
        
        // Clear cache
        file_io_enhanced_clear_cache();
        
        // Destroy mutex
        pthread_mutex_destroy(&g_cache_manager->mutex);
        
        // Free memory
        free(g_cache_manager->buckets);
        free(g_cache_manager);
        g_cache_manager = NULL;
    }
    
    atomic_store(&g_initialized, 0);
    log_info("Enhanced file I/O module destroyed");
}

// Get file I/O statistics
void file_io_enhanced_get_stats(file_io_stats_t *stats) {
    if (stats) {
        memcpy(stats, &g_stats, sizeof(file_io_stats_t));
    }
}

// Reset file I/O statistics
void file_io_enhanced_reset_stats(void) {
    memset(&g_stats, 0, sizeof(g_stats));
}

// Print file I/O statistics
void file_io_enhanced_print_stats(void) {
    size_t current_size, max_size, hit_count, miss_count;
    file_io_enhanced_get_cache_info(&current_size, &max_size, &hit_count, &miss_count);
    
    log_info("=== File I/O Statistics ===");
    log_info("Total requests: %lu", atomic_load(&g_stats.total_requests));
    log_info("Cache hits: %lu", atomic_load(&g_stats.cache_hits));
    log_info("Cache misses: %lu", atomic_load(&g_stats.cache_misses));
    log_info("Sendfile requests: %lu", atomic_load(&g_stats.sendfile_requests));
    log_info("Mmap requests: %lu", atomic_load(&g_stats.mmap_requests));
    log_info("Async requests: %lu", atomic_load(&g_stats.async_requests));
    log_info("Total bytes sent: %lu", atomic_load(&g_stats.total_bytes_sent));
    log_info("Total read time: %lu ns", atomic_load(&g_stats.total_read_time));
    log_info("Total send time: %lu ns", atomic_load(&g_stats.total_send_time));
    log_info("Cache usage: %zu/%zu bytes (%.1f%%)", 
             current_size, max_size, (double)current_size / max_size * 100);
    log_info("Cache hit rate: %.1f%%", 
             (hit_count + miss_count) > 0 ? (double)hit_count / (hit_count + miss_count) * 100 : 0);
}

// Get file from cache
void *file_io_enhanced_get_from_cache(const char *file_path, size_t *size) {
    if (!g_cache_manager || !file_path) {
        return NULL;
    }
    
    size_t hash = hash_string(file_path) % g_cache_manager->bucket_count;
    
    pthread_mutex_lock(&g_cache_manager->mutex);
    
    file_cache_item_t *item = g_cache_manager->buckets[hash];
    while (item != NULL) {
        if (strcmp(item->path, file_path) == 0 && atomic_load(&item->is_valid)) {
            // Update access time
            item->access_time = time(NULL);
            atomic_fetch_add(&item->ref_count, 1);
            
            if (size) *size = item->size;
            
            pthread_mutex_unlock(&g_cache_manager->mutex);
            atomic_fetch_add(&g_stats.cache_hits, 1);
            return item->data;
        }
        item = item->next;
    }
    
    pthread_mutex_unlock(&g_cache_manager->mutex);
    atomic_fetch_add(&g_stats.cache_misses, 1);
    return NULL;
}

// Add file to cache
int file_io_enhanced_add_to_cache(const char *file_path, const void *data, size_t size) {
    if (!g_cache_manager || !file_path || !data) {
        return -1;
    }
    
    // Check if file size exceeds limit
    if (size > (size_t)(g_config.max_file_size * 1024 * 1024)) {
        return -1;
    }
    
    size_t hash = hash_string(file_path) % g_cache_manager->bucket_count;
    
    pthread_mutex_lock(&g_cache_manager->mutex);
    
    // Check if already exists
    file_cache_item_t *item = g_cache_manager->buckets[hash];
    while (item != NULL) {
        if (strcmp(item->path, file_path) == 0) {
            // Update existing item
            free(item->data);
            item->data = malloc(size);
            if (!item->data) {
                pthread_mutex_unlock(&g_cache_manager->mutex);
                return -1;
            }
            memcpy(item->data, data, size);
            item->size = size;
            item->mtime = time(NULL);
            item->access_time = time(NULL);
            atomic_store(&item->is_valid, 1);
            
            pthread_mutex_unlock(&g_cache_manager->mutex);
            return 0;
        }
        item = item->next;
    }
    
    // Create new item
    item = malloc(sizeof(file_cache_item_t));
    if (!item) {
        pthread_mutex_unlock(&g_cache_manager->mutex);
        return -1;
    }
    
    item->path = strdup(file_path);
    item->data = malloc(size);
    if (!item->path || !item->data) {
        free(item->path);
        free(item->data);
        free(item);
        pthread_mutex_unlock(&g_cache_manager->mutex);
        return -1;
    }
    
    memcpy(item->data, data, size);
    item->size = size;
    item->mtime = time(NULL);
    item->access_time = time(NULL);
    atomic_store(&item->ref_count, 1);
    atomic_store(&item->is_valid, 1);
    
    // Add to linked list head
    item->next = g_cache_manager->buckets[hash];
    g_cache_manager->buckets[hash] = item;
    g_cache_manager->current_size += size;
    
    pthread_mutex_unlock(&g_cache_manager->mutex);
    return 0;
}

// Remove file from cache
void file_io_enhanced_remove_from_cache(const char *file_path) {
    if (!g_cache_manager || !file_path) {
        return;
    }
    
    size_t hash = hash_string(file_path) % g_cache_manager->bucket_count;
    
    pthread_mutex_lock(&g_cache_manager->mutex);
    
    file_cache_item_t *item = g_cache_manager->buckets[hash];
    file_cache_item_t *prev = NULL;
    
    while (item != NULL) {
        if (strcmp(item->path, file_path) == 0) {
            if (prev == NULL) {
                g_cache_manager->buckets[hash] = item->next;
            } else {
                prev->next = item->next;
            }
            
            g_cache_manager->current_size -= item->size;
            free(item->path);
            free(item->data);
            free(item);
            break;
        }
        prev = item;
        item = item->next;
    }
    
    pthread_mutex_unlock(&g_cache_manager->mutex);
}

// Clear cache
void file_io_enhanced_clear_cache(void) {
    if (!g_cache_manager) {
        return;
    }
    
    pthread_mutex_lock(&g_cache_manager->mutex);
    
    for (size_t i = 0; i < g_cache_manager->bucket_count; i++) {
        file_cache_item_t *item = g_cache_manager->buckets[i];
        while (item != NULL) {
            file_cache_item_t *next = item->next;
            free(item->path);
            free(item->data);
            free(item);
            item = next;
        }
        g_cache_manager->buckets[i] = NULL;
    }
    
    g_cache_manager->current_size = 0;
    
    pthread_mutex_unlock(&g_cache_manager->mutex);
}

// Send file using sendfile
int file_io_enhanced_send_file_sendfile(int client_fd, const char *file_path, size_t *sent_bytes) {
    if (!g_config.enable_sendfile) {
        return -1;
    }
    
    uint64_t start_time = get_time_ns();
    
    int file_fd = open(file_path, O_RDONLY);
    if (file_fd < 0) {
        return -1;
    }
    
    struct stat st;
    if (fstat(file_fd, &st) < 0) {
        close(file_fd);
        return -1;
    }
    
    size_t total_sent = 0;
    off_t offset = 0;
    
#if defined(__linux__)
    // Linux sendfile
    while (total_sent < (size_t)st.st_size) {
        ssize_t n = sendfile(client_fd, file_fd, &offset, st.st_size - total_sent);
        if (n > 0) {
            total_sent += n;
        } else if (n == 0) {
            break;
        } else {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(1000);
                continue;
            }
            close(file_fd);
            return -1;
        }
    }
#elif defined(__APPLE__)
    // macOS sendfile
    off_t len = st.st_size;
    int result = sendfile(file_fd, client_fd, offset, &len, NULL, 0);
    if (result == 0) {
        total_sent = len;
    } else {
        close(file_fd);
        return -1;
    }
#else
    close(file_fd);
    return -1;
#endif
    
    close(file_fd);
    
    if (sent_bytes) *sent_bytes = total_sent;
    
    atomic_fetch_add(&g_stats.sendfile_requests, 1);
    atomic_fetch_add(&g_stats.total_bytes_sent, total_sent);
    atomic_fetch_add(&g_stats.total_send_time, get_time_ns() - start_time);
    
    return 0;
}

// Send file using mmap
int file_io_enhanced_send_file_mmap(int client_fd, const char *file_path, size_t *sent_bytes) {
    if (!g_config.enable_mmap) {
        return -1;
    }
    
    uint64_t start_time = get_time_ns();
    
    int file_fd = open(file_path, O_RDONLY);
    if (file_fd < 0) {
        return -1;
    }
    
    struct stat st;
    if (fstat(file_fd, &st) < 0) {
        close(file_fd);
        return -1;
    }
    
    // Map file using mmap
    void *addr = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, file_fd, 0);
    if (addr == MAP_FAILED) {
        close(file_fd);
        return -1;
    }
    
    // Send file content
    size_t total_sent = 0;
    ssize_t bytes_sent;
    
    while (total_sent < (size_t)st.st_size) {
        bytes_sent = write(client_fd, (char *)addr + total_sent, st.st_size - total_sent);
        if (bytes_sent > 0) {
            total_sent += bytes_sent;
        } else if (bytes_sent == 0) {
            break;
        } else {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(1000);
                continue;
            }
            munmap(addr, st.st_size);
            close(file_fd);
            return -1;
        }
    }
    
    munmap(addr, st.st_size);
    close(file_fd);
    
    if (sent_bytes) *sent_bytes = total_sent;
    
    atomic_fetch_add(&g_stats.mmap_requests, 1);
    atomic_fetch_add(&g_stats.total_bytes_sent, total_sent);
    atomic_fetch_add(&g_stats.total_send_time, get_time_ns() - start_time);
    
    return 0;
}

// Zero-copy file sending (automatically select optimal method)
int file_io_enhanced_send_file(int client_fd, const char *file_path, size_t *sent_bytes) {
    if (!atomic_load(&g_initialized)) {
        return -1;
    }
    
    atomic_fetch_add(&g_stats.total_requests, 1);
    
    // First try to get from cache
    size_t cached_size;
    void *cached_data = file_io_enhanced_get_from_cache(file_path, &cached_size);
    if (cached_data) {
        // Send from cache
        uint64_t start_time = get_time_ns();
        
        size_t total_sent = 0;
        ssize_t bytes_sent;
        
        while (total_sent < cached_size) {
            bytes_sent = write(client_fd, (char *)cached_data + total_sent, cached_size - total_sent);
            if (bytes_sent > 0) {
                total_sent += bytes_sent;
            } else if (bytes_sent == 0) {
                break;
            } else {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    usleep(1000);
                    continue;
                }
                return -1;
            }
        }
        
        if (sent_bytes) *sent_bytes = total_sent;
        atomic_fetch_add(&g_stats.total_bytes_sent, total_sent);
        atomic_fetch_add(&g_stats.total_send_time, get_time_ns() - start_time);
        
        return 0;
    }
    
    // Get file information
    struct stat st;
    if (stat(file_path, &st) < 0) {
        return -1;
    }
    
    // Select optimal method based on file size
    if (st.st_size <= 1024 * 1024) { // Less than 1MB, use sendfile
        return file_io_enhanced_send_file_sendfile(client_fd, file_path, sent_bytes);
    } else { // Greater than 1MB, use mmap
        return file_io_enhanced_send_file_mmap(client_fd, file_path, sent_bytes);
    }
}

// Preload file to cache
int file_io_enhanced_preload_file(const char *file_path) {
    if (!atomic_load(&g_initialized)) {
        return -1;
    }
    
    // Check if already in cache
    if (file_io_enhanced_is_cached(file_path)) {
        return 0;
    }
    
    int file_fd = open(file_path, O_RDONLY);
    if (file_fd < 0) {
        return -1;
    }
    
    struct stat st;
    if (fstat(file_fd, &st) < 0) {
        close(file_fd);
        return -1;
    }
    
    // Check file size
    if (st.st_size > (off_t)(g_config.max_file_size * 1024 * 1024)) {
        close(file_fd);
        return -1;
    }
    
    // Read file content
    void *data = malloc(st.st_size);
    if (!data) {
        close(file_fd);
        return -1;
    }
    
    ssize_t bytes_read = read(file_fd, data, st.st_size);
    close(file_fd);
    
    if (bytes_read != st.st_size) {
        free(data);
        return -1;
    }
    
    // Add to cache
    int result = file_io_enhanced_add_to_cache(file_path, data, st.st_size);
    free(data);
    
    return result;
}

// Batch preload files
int file_io_enhanced_preload_files(const char **file_paths, int count) {
    if (!file_paths || count <= 0) {
        return -1;
    }
    
    int success_count = 0;
    for (int i = 0; i < count; i++) {
        if (file_io_enhanced_preload_file(file_paths[i]) == 0) {
            success_count++;
        }
    }
    
    return success_count;
}

// Check if file is in cache
int file_io_enhanced_is_cached(const char *file_path) {
    if (!g_cache_manager || !file_path) {
        return 0;
    }
    
    size_t hash = hash_string(file_path) % g_cache_manager->bucket_count;
    
    pthread_mutex_lock(&g_cache_manager->mutex);
    
    file_cache_item_t *item = g_cache_manager->buckets[hash];
    while (item != NULL) {
        if (strcmp(item->path, file_path) == 0 && atomic_load(&item->is_valid)) {
            pthread_mutex_unlock(&g_cache_manager->mutex);
            return 1;
        }
        item = item->next;
    }
    
    pthread_mutex_unlock(&g_cache_manager->mutex);
    return 0;
}

// Get cache usage information
void file_io_enhanced_get_cache_info(size_t *current_size, size_t *max_size, 
                                    size_t *hit_count, size_t *miss_count) {
    if (current_size) *current_size = g_cache_manager ? g_cache_manager->current_size : 0;
    if (max_size) *max_size = g_cache_manager ? g_cache_manager->max_size : 0;
    if (hit_count) *hit_count = atomic_load(&g_stats.cache_hits);
    if (miss_count) *miss_count = atomic_load(&g_stats.cache_misses);
}

// Get file information (cache-friendly)
int file_io_enhanced_get_file_info(const char *file_path, struct stat *st) {
    if (!file_path || !st) {
        return -1;
    }
    
    return stat(file_path, st);
} 
/**
 * Memory Pool Management Module Implementation - High Performance Version
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include "../include/memory_pool.h"
#include "../include/logger.h"

#define MIN_BLOCK_SIZE 64  // Minimum memory block size
#define DEFAULT_ALIGNMENT 8  // Default memory alignment size
#define SEGMENT_COUNT 16  // Number of segments
#define MAX_SEGMENT_SIZE (1024 * 1024)  // Maximum size per segment

// Memory segment structure
typedef struct memory_segment {
    memory_block_t *blocks;
    pthread_mutex_t mutex;
    atomic_size_t total_size;
    atomic_size_t used_size;
    atomic_int ref_count;
    size_t segment_id;
} memory_segment_t;

// Memory pool statistics and monitoring
typedef struct memory_pool_stats {
    atomic_size_t total_allocations;
    atomic_size_t total_deallocations;
    atomic_size_t peak_usage;
    atomic_size_t current_usage;
    time_t last_cleanup;
} memory_pool_stats_t;

// High performance memory pool structure
struct memory_pool {
    memory_segment_t segments[SEGMENT_COUNT];
    pthread_mutex_t global_mutex;
    atomic_size_t total_size;
    atomic_size_t used_size;
    atomic_int segment_count;
    size_t initial_size;
    memory_pool_stats_t stats;  // Add statistics
    pthread_t cleanup_thread;   // Auto cleanup thread
    atomic_int cleanup_running; // Cleanup thread running flag
};

// Internal function: allocate a new memory block
static memory_block_t *allocate_block(size_t size) {
    memory_block_t *block = (memory_block_t *)malloc(sizeof(memory_block_t));
    if (block == NULL) {
        log_error("Unable to allocate memory block structure");
        return NULL;
    }
    
    // Allocate actual data memory
    block->data = malloc(size);
    if (block->data == NULL) {
        log_error("Unable to allocate memory block data area");
        free(block);
        return NULL;
    }
    
    block->size = size;
    block->in_use = 0;
    block->next = NULL;
    
    return block;
}

// Calculate segment ID
static size_t get_segment_id(size_t size) {
    if (size <= 256) return 0;
    if (size <= 512) return 1;
    if (size <= 1024) return 2;
    if (size <= 2048) return 3;
    if (size <= 4096) return 4;
    if (size <= 8192) return 5;
    if (size <= 16384) return 6;
    if (size <= 32768) return 7;
    if (size <= 65536) return 8;
    if (size <= 131072) return 9;
    if (size <= 262144) return 10;
    if (size <= 524288) return 11;
    if (size <= 1048576) return 12;
    return 13; // Large memory blocks
}

// Create memory pool
memory_pool_t *create_memory_pool(size_t initial_size) {
    // Ensure initial size is reasonable
    if (initial_size < MIN_BLOCK_SIZE) {
        initial_size = MIN_BLOCK_SIZE;
    }
    
    // Allocate memory pool structure
    memory_pool_t *pool = (memory_pool_t *)malloc(sizeof(memory_pool_t));
    if (pool == NULL) {
        log_error("Unable to create memory pool");
        return NULL;
    }
    
    // Initialize memory pool
    memset(pool, 0, sizeof(memory_pool_t));
    pool->initial_size = initial_size;
    
    // Initialize global mutex
    if (pthread_mutex_init(&pool->global_mutex, NULL) != 0) {
        log_error("Unable to initialize memory pool global mutex");
        free(pool);
        return NULL;
    }
    
    // Initialize all segments
    for (int i = 0; i < SEGMENT_COUNT; i++) {
        memory_segment_t *segment = &pool->segments[i];
        
        if (pthread_mutex_init(&segment->mutex, NULL) != 0) {
            log_error("Unable to initialize memory pool segment mutex %d", i);
            // Clean up already initialized segments
            for (int j = 0; j < i; j++) {
                pthread_mutex_destroy(&pool->segments[j].mutex);
            }
            pthread_mutex_destroy(&pool->global_mutex);
            free(pool);
            return NULL;
        }
        
        segment->blocks = NULL;
        segment->segment_id = i;
        atomic_init(&segment->total_size, 0);
        atomic_init(&segment->used_size, 0);
        atomic_init(&segment->ref_count, 0);
    }
    
    // Allocate initial memory block to appropriate segment
    size_t segment_id = get_segment_id(initial_size);
    memory_segment_t *segment = &pool->segments[segment_id];
    
    pthread_mutex_lock(&segment->mutex);
    memory_block_t *initial_block = allocate_block(initial_size);
    if (initial_block != NULL) {
        segment->blocks = initial_block;
        atomic_store(&segment->total_size, initial_size);
        atomic_fetch_add(&pool->total_size, initial_size);
        atomic_fetch_add(&pool->segment_count, 1);
    }
    pthread_mutex_unlock(&segment->mutex);
    
    if (initial_block == NULL) {
        log_error("Unable to allocate initial memory block");
        // Clean up resources
        for (int i = 0; i < SEGMENT_COUNT; i++) {
            pthread_mutex_destroy(&pool->segments[i].mutex);
        }
        pthread_mutex_destroy(&pool->global_mutex);
        free(pool);
        return NULL;
    }
    
    log_debug("High performance memory pool created successfully, initial size: %zu bytes", initial_size);
    return pool;
}

// Allocate memory from memory pool
void *pool_malloc(memory_pool_t *pool, size_t size) {
    if (pool == NULL || size == 0) {
        return NULL;
    }
    
    // Align memory size
    size_t aligned_size = (size + DEFAULT_ALIGNMENT - 1) & ~(DEFAULT_ALIGNMENT - 1);
    
    // Get segment ID
    size_t segment_id = get_segment_id(aligned_size);
    
    // If segment ID is out of range, memory block is too large, need special handling or error
    if (segment_id >= SEGMENT_COUNT) {
        log_error("Memory block size exceeds maximum supported size: %zu", aligned_size);
        return NULL;
    }
    
    memory_segment_t *segment = &pool->segments[segment_id];
    
    pthread_mutex_lock(&segment->mutex);
    
    // Find available memory block
    memory_block_t *block = segment->blocks;
    memory_block_t *prev = NULL;
    memory_block_t *best_fit = NULL;
    size_t best_fit_size = (size_t)-1;  // Initialize to maximum value
    
    // Use best fit algorithm to find suitable block
    while (block != NULL) {
        if (!block->in_use && block->size >= aligned_size) {
            // Found an available block
            if (block->size < best_fit_size) {
                best_fit = block;
                best_fit_size = block->size;
                
                // If found perfectly matching block, use it immediately
                if (block->size == aligned_size) {
                    break;
                }
            }
        }
        prev = block;
        block = block->next;
    }
    
    // If found suitable block
    if (best_fit != NULL) {
        best_fit->in_use = 1;
        atomic_fetch_add(&segment->used_size, best_fit->size);
        atomic_fetch_add(&pool->used_size, best_fit->size);
        pthread_mutex_unlock(&segment->mutex);
        
        // Zero out memory block
        memset(best_fit->data, 0, best_fit->size);
        return best_fit->data;
    }
    
    // No suitable block found, allocate new block
    size_t new_block_size = aligned_size > MIN_BLOCK_SIZE ? aligned_size : MIN_BLOCK_SIZE;
    memory_block_t *new_block = allocate_block(new_block_size);
    
    if (new_block == NULL) {
        pthread_mutex_unlock(&segment->mutex);
        return NULL;
    }
    
    // Add new block to end of linked list
    if (prev == NULL) {
        segment->blocks = new_block;
    } else {
        prev->next = new_block;
    }
    
    // Update statistics
    new_block->in_use = 1;
    atomic_fetch_add(&segment->total_size, new_block_size);
    atomic_fetch_add(&pool->total_size, new_block_size);
    atomic_fetch_add(&segment->used_size, new_block_size);
    atomic_fetch_add(&pool->used_size, new_block_size);
    
    pthread_mutex_unlock(&segment->mutex);
    
    // Zero out memory block
    memset(new_block->data, 0, new_block_size);
    return new_block->data;
}

// Free memory to memory pool
void pool_free(memory_pool_t *pool, void *ptr) {
    if (pool == NULL || ptr == NULL) {
        return;
    }
    
    // Get memory block containing the pointer
    memory_block_t *block = (memory_block_t *)ptr;
    if (block == NULL) {
        log_warn("Attempting to free null pointer or invalid pointer: %p", ptr);
        return;
    }

    // Get segment ID
    size_t segment_id = get_segment_id(block->size);
    
    // If segment ID is out of range, memory block is too large, need special handling or error
    if (segment_id >= SEGMENT_COUNT) {
        log_warn("Attempting to free memory block outside memory pool support range: %p", ptr);
        return;
    }

    memory_segment_t *segment = &pool->segments[segment_id];
    
    pthread_mutex_lock(&segment->mutex);
    
    // Find corresponding memory block
    memory_block_t *current = segment->blocks;
    
    while (current != NULL) {
        if (current->data == ptr) {
            // Found corresponding block
            if (current->in_use) {
                current->in_use = 0;
                atomic_fetch_sub(&segment->used_size, current->size);
                atomic_fetch_sub(&pool->used_size, current->size);
            }
            pthread_mutex_unlock(&segment->mutex);
            return;
        }
        current = current->next;
    }
    
    // No corresponding block found, may be illegal pointer
    //log_warn("Attempting to free memory not allocated by memory pool: %p", ptr);
    
    pthread_mutex_unlock(&segment->mutex);
}

// Destroy memory pool
void destroy_memory_pool(memory_pool_t *pool) {
    if (pool == NULL) {
        return;
    }
    
    // Destroy all segment mutexes
    for (int i = 0; i < SEGMENT_COUNT; i++) {
        pthread_mutex_destroy(&pool->segments[i].mutex);
    }
    
    // Destroy global mutex
    pthread_mutex_destroy(&pool->global_mutex);
    
    // Free memory pool structure
    free(pool);
    
    log_debug("Memory pool destroyed");
}

// Get memory pool statistics
void get_pool_stats(memory_pool_t *pool, size_t *total_size, size_t *used_size) {
    if (pool == NULL) {
        if (total_size) *total_size = 0;
        if (used_size) *used_size = 0;
        return;
    }
    
    // Use atomic operations to get statistics
    atomic_size_t atomic_total = atomic_load(&pool->total_size);
    atomic_size_t atomic_used = atomic_load(&pool->used_size);
    
    if (total_size) *total_size = atomic_total;
    if (used_size) *used_size = atomic_used;
}

// Memory pool compression function - free unused memory blocks
int compress_memory_pool(memory_pool_t *pool) {
    if (pool == NULL) {
        return -1;
    }
    
    // Get global mutex
    pthread_mutex_lock(&pool->global_mutex);
    
    // Traverse all segments
    int freed_blocks = 0;
    size_t freed_size = 0;
    
    for (int i = 0; i < SEGMENT_COUNT; i++) {
        memory_segment_t *segment = &pool->segments[i];
        
        pthread_mutex_lock(&segment->mutex);
        
        memory_block_t *block = segment->blocks;
        memory_block_t *prev = NULL;
        memory_block_t *next;
        
        // Calculate current usage ratio
        double usage_ratio = (double)atomic_load(&segment->used_size) / (double)atomic_load(&segment->total_size);
        
        // More aggressive compression strategy: if usage ratio is below 50%, perform compression
        int aggressive_compress = (usage_ratio < 0.5);
        
        // Number of free blocks to keep by size category - more aggressive limits
        int small_free_blocks = 0;   // <= 1KB
        int medium_free_blocks = 0;  // <= 8KB
        int large_free_blocks = 0;   // > 8KB
        
        // Maximum number of free blocks to keep - more aggressive limits
        int max_small_blocks = aggressive_compress ? 1 : 2;
        int max_medium_blocks = aggressive_compress ? 1 : 2;
        int max_large_blocks = aggressive_compress ? 0 : 1;
        
        while (block != NULL) {
            next = block->next;
            
            if (!block->in_use) {
                int should_free = 0;
                
                // Decide whether to keep based on block size
                if (block->size <= 1024) {
                    if (small_free_blocks >= max_small_blocks) {
                        should_free = 1;
                    } else {
                        small_free_blocks++;
                    }
                } else if (block->size <= 8192) {
                    if (medium_free_blocks >= max_medium_blocks) {
                        should_free = 1;
                    } else {
                        medium_free_blocks++;
                    }
                } else {
                    if (large_free_blocks >= max_large_blocks) {
                        should_free = 1;
                    } else {
                        large_free_blocks++;
                    }
                }
                
                // If decided to free this block
                if (should_free) {
                    if (prev == NULL) {
                        segment->blocks = next;
                    } else {
                        prev->next = next;
                    }
                    
                    freed_size += block->size;
                    atomic_fetch_sub(&segment->total_size, block->size);
                    atomic_fetch_sub(&pool->total_size, block->size);
                    
                    free(block->data);
                    free(block);
                    freed_blocks++;
                } else {
                    prev = block;
                }
            } else {
                prev = block;
            }
            
            block = next;
        }
        
        pthread_mutex_unlock(&segment->mutex);
    }
    
    pthread_mutex_unlock(&pool->global_mutex);
    
    if (freed_blocks > 0) {
        log_info("Memory pool compression completed, freed %d blocks, total %zu bytes, current usage ratio: %.2f%%", 
                 freed_blocks, freed_size, (double)atomic_load(&pool->used_size) / (double)atomic_load(&pool->total_size) * 100);
    }
    return freed_blocks;
}

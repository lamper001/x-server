/**
 * Thread Pool Implementation Module
 */

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include "../include/thread_pool.h"
#include "../include/logger.h"

// Task structure
typedef struct {
    void (*function)(void *);  // Task function
    void *argument;            // Task argument
} thread_pool_task_t;

// Thread pool structure
struct thread_pool {
    pthread_mutex_t lock;           // Mutex lock
    pthread_cond_t notify;          // Condition variable
    pthread_t *threads;             // Worker threads array
    thread_pool_task_t *queue;      // Task queue
    int thread_count;               // Thread count
    int queue_size;                 // Queue size
    int head;                       // Queue head
    int tail;                       // Queue tail
    int count;                      // Current task count
    int shutdown;                   // Shutdown flag
    int started;                    // Started thread count
};

// Thread worker function
static void *thread_worker(void *arg) {
    thread_pool_t *pool = (thread_pool_t *)arg;
    thread_pool_task_t task;

    while (1) {
        // Get task
        pthread_mutex_lock(&(pool->lock));

        // Wait for task or shutdown signal
        while ((pool->count == 0) && (!pool->shutdown)) {
            pthread_cond_wait(&(pool->notify), &(pool->lock));
        }

        // Check if thread needs to be shut down
        if ((pool->shutdown == 1) && (pool->count == 0)) {
            break;
        }

        // Get task
        task.function = pool->queue[pool->head].function;
        task.argument = pool->queue[pool->head].argument;
        pool->head = (pool->head + 1) % pool->queue_size;
        pool->count--;

        pthread_mutex_unlock(&(pool->lock));

        // Execute task
        (*(task.function))(task.argument);
    }

    // Thread exit
    pool->started--;
    pthread_mutex_unlock(&(pool->lock));
    pthread_exit(NULL);
    return NULL;
}

// Create thread pool
thread_pool_t *thread_pool_create(int thread_count, int queue_size) {
    thread_pool_t *pool;
    int i;

    // Check parameters
    if (thread_count <= 0 || queue_size <= 0) {
        return NULL;
    }

    // Allocate memory
    pool = (thread_pool_t *)malloc(sizeof(thread_pool_t));
    if (pool == NULL) {
        log_error("Failed to allocate thread pool memory");
        return NULL;
    }

    // Initialize thread pool
    pool->thread_count = thread_count;
    pool->queue_size = queue_size;
    pool->head = pool->tail = pool->count = 0;
    pool->shutdown = pool->started = 0;

    // Allocate thread and task queue memory
    pool->threads = (pthread_t *)malloc(sizeof(pthread_t) * thread_count);
    pool->queue = (thread_pool_task_t *)malloc(sizeof(thread_pool_task_t) * queue_size);

    // Check memory allocation
    if (pool->threads == NULL || pool->queue == NULL) {
        log_error("Failed to allocate thread pool queue memory");
        if (pool->threads) free(pool->threads);
        if (pool->queue) free(pool->queue);
        free(pool);
        return NULL;
    }

    // Initialize mutex and condition variable
    if (pthread_mutex_init(&(pool->lock), NULL) != 0 ||
        pthread_cond_init(&(pool->notify), NULL) != 0) {
        log_error("Failed to initialize thread pool lock or condition variable");
        free(pool->threads);
        free(pool->queue);
        free(pool);
        return NULL;
    }

    // Create worker threads
    for (i = 0; i < thread_count; i++) {
        if (pthread_create(&(pool->threads[i]), NULL, thread_worker, (void*)pool) != 0) {
            thread_pool_destroy(pool, 0);
            return NULL;
        }
        pool->started++;
        log_debug("Thread pool created worker thread %d", i);
    }

    log_info("Thread pool initialized successfully, thread count: %d, queue size: %d", thread_count, queue_size);
    return pool;
}

// Add task to thread pool
int thread_pool_add(thread_pool_t *pool, void (*function)(void *), void *argument) {
    int err = 0;
    int next;

    if (pool == NULL || function == NULL) {
        return THREAD_POOL_INVALID;
    }

    // Acquire lock
    if (pthread_mutex_lock(&(pool->lock)) != 0) {
        return THREAD_POOL_LOCK_FAILURE;
    }

    // Calculate next available position
    next = (pool->tail + 1) % pool->queue_size;

    // Check if queue is full
    if (pool->count == pool->queue_size) {
        err = THREAD_POOL_QUEUE_FULL;
        log_warn("Thread pool queue is full");
    }
    // Check if thread pool is shut down
    else if (pool->shutdown) {
        err = THREAD_POOL_SHUTDOWN;
    }
    // Add task to queue
    else {
        pool->queue[pool->tail].function = function;
        pool->queue[pool->tail].argument = argument;
        pool->tail = next;
        pool->count++;

        // Notify worker threads
        if (pthread_cond_signal(&(pool->notify)) != 0) {
            err = THREAD_POOL_LOCK_FAILURE;
        }
    }

    // Release lock
    if (pthread_mutex_unlock(&(pool->lock)) != 0) {
        err = THREAD_POOL_LOCK_FAILURE;
    }

    return err;
}

// Destroy thread pool
int thread_pool_destroy(thread_pool_t *pool, int flags) {
    int i, err = 0;

    if (pool == NULL) {
        return THREAD_POOL_INVALID;
    }

    // Acquire lock
    if (pthread_mutex_lock(&(pool->lock)) != 0) {
        return THREAD_POOL_LOCK_FAILURE;
    }

    // Check if thread pool is already shut down
    if (pool->shutdown) {
        err = THREAD_POOL_SHUTDOWN;
    }
    else {
        // Set shutdown flag
        pool->shutdown = 1;

        // Wake up all worker threads
        if ((pthread_cond_broadcast(&(pool->notify)) != 0) ||
            (pthread_mutex_unlock(&(pool->lock)) != 0)) {
            err = THREAD_POOL_LOCK_FAILURE;
            return err;
        }

        // Wait for all threads to complete
        for (i = 0; i < pool->thread_count; i++) {
            if (pthread_join(pool->threads[i], NULL) != 0) {
                err = THREAD_POOL_THREAD_FAILURE;
            }
        }
    }

    // Release resources
    if (!err || flags) {
        pthread_mutex_destroy(&(pool->lock));
        pthread_cond_destroy(&(pool->notify));
        free(pool->threads);
        free(pool->queue);
        free(pool);
    }

    return err;
}
/**
 * Unified High-Performance Event-Driven I/O Framework Implementation
 * Combines advantages of original and enhanced versions for optimal performance and stability
 * References Nginx and epoll/kqueue best practices
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <sys/time.h>
#include <math.h>

#include "../include/event_loop.h"
#include "../include/logger.h"

#ifdef __linux__
#include <sys/epoll.h>
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
#include <sys/event.h>
#else
#error "不支持的操作系统"
#endif

// High-performance spinlock structure
typedef struct spinlock {
    atomic_int locked;
    atomic_int contention_count;  // Contention statistics
} spinlock_t;

// Unified event handler structure
struct event_handler {
    int fd;                                    // File descriptor
    event_callback_t read_cb;                  // Read event callback
    event_callback_t write_cb;                 // Write event callback
    void *arg;                                 // Callback argument
    int events;                                // Monitored events
    atomic_int ref_count;                      // Reference count
    atomic_int active;                         // Active status
    struct timespec last_activity;             // Last activity time
    uint64_t processing_count;                 // Processing count
    double avg_processing_time;                // Average processing time
};

// Hash table node
typedef struct handler_node {
    int fd;
    event_handler_t *handler;
    struct handler_node *next;
    atomic_int ref_count;
} handler_node_t;

// Unified event loop structure
struct event_loop {
#ifdef __linux__
    int epoll_fd;                              // epoll file descriptor
    struct epoll_event *events;                // epoll events array
#else
    int kqueue_fd;                             // kqueue file descriptor
    struct kevent *events;                     // kqueue events array
#endif
    int max_events;                            // Maximum events
    int batch_size;                            // Batch size
    int timeout_ms;                            // Timeout (milliseconds)
    atomic_int stop;                           // Stop flag
    pthread_t thread_id;                       // Event loop thread ID
    
    // Hash table management (high-performance lookup)
    handler_node_t **handler_table;            // Event handler hash table
    int table_size;                            // Hash table size
    pthread_rwlock_t *rwlocks;                 // Segmented read-write locks
    atomic_int handler_count;                  // Handler count
    atomic_int active_handlers;                // Active handler count
    
    // Performance statistics
    _Atomic uint64_t total_events_processed;    // Total events processed
    _Atomic uint64_t batch_events_processed;    // Batch events processed
    _Atomic uint64_t error_count;               // Error count
    _Atomic uint64_t timeout_count;             // Timeout count
    _Atomic uint64_t lock_contention;           // Lock contention statistics
    
    // Time statistics
    double avg_event_processing_time;          // Average event processing time
    double max_event_processing_time;          // Maximum event processing time
    double min_event_processing_time;          // Minimum event processing time
    spinlock_t stats_lock;                     // Statistics lock
    
    // Main lock
    pthread_mutex_t mutex;                     // Main mutex
};

// Spinlock operations (optimized version)
static inline void spinlock_init(spinlock_t *lock) {
    atomic_init(&lock->locked, 0);
    atomic_init(&lock->contention_count, 0);
}

static inline void spinlock_lock(spinlock_t *lock) {
    int expected = 0;
    int spin_count = 0;
    
    while (!atomic_compare_exchange_weak(&lock->locked, &expected, 1)) {
        expected = 0;
        spin_count++;
        
        if (spin_count > 100) {
            // Too much spinning, yield CPU
            sched_yield();
            spin_count = 0;
            atomic_fetch_add(&lock->contention_count, 1);
        } else {
            // Brief spinning - cross-platform compatible
#ifdef __x86_64__
            __asm__ volatile("pause");
#elif defined(__aarch64__) || defined(__arm64__)
            __asm__ volatile("yield");
#else
            // Generic platform uses simple delay
            for (volatile int i = 0; i < 10; i++) {
                // Empty loop as delay
            }
#endif
        }
    }
}

static inline void spinlock_unlock(spinlock_t *lock) {
    atomic_store(&lock->locked, 0);
}

// Get current time (microseconds)
static inline uint64_t get_time_us() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
}

// Optimized hash function
static unsigned int hash_fd(int fd) {
    unsigned int hash = (unsigned int)fd;
    hash = ((hash << 13) ^ hash) ^ (hash >> 17);
    hash = ((hash << 5) ^ hash) ^ (hash >> 3);
    return hash;
}

// Get hash table index
static unsigned int get_table_index(event_loop_t *loop, int fd) {
    return hash_fd(fd) % loop->table_size;
}

// Add handler to hash table
static void add_handler_to_table(event_loop_t *loop, int fd, event_handler_t *handler) {
    unsigned int index = get_table_index(loop, fd);
    
    pthread_rwlock_wrlock(&loop->rwlocks[index]);
    
    handler_node_t *node = malloc(sizeof(handler_node_t));
    if (node) {
        node->fd = fd;
        node->handler = handler;
        node->next = loop->handler_table[index];
        atomic_init(&node->ref_count, 1);
        loop->handler_table[index] = node;
        atomic_fetch_add(&loop->handler_count, 1);
        atomic_fetch_add(&loop->active_handlers, 1);
    }
    
    pthread_rwlock_unlock(&loop->rwlocks[index]);
}

// Remove handler from hash table
static event_handler_t *remove_handler_from_table(event_loop_t *loop, int fd) {
    unsigned int index = get_table_index(loop, fd);
    
    pthread_rwlock_wrlock(&loop->rwlocks[index]);
    
    handler_node_t **current = &loop->handler_table[index];
    event_handler_t *handler = NULL;
    
    while (*current) {
        if ((*current)->fd == fd) {
            handler_node_t *to_remove = *current;
            handler = to_remove->handler;
            *current = to_remove->next;
            
            atomic_fetch_add(&handler->ref_count, 1);
            
            free(to_remove);
            atomic_fetch_sub(&loop->handler_count, 1);
            atomic_fetch_sub(&loop->active_handlers, 1);
            break;
        }
        current = &(*current)->next;
    }
    
    pthread_rwlock_unlock(&loop->rwlocks[index]);
    return handler;
}

// Set file descriptor to non-blocking mode
static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        log_error("Failed to get file descriptor flags: %s", strerror(errno));
        return -1;
    }
    
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        log_error("Failed to set non-blocking mode: %s", strerror(errno));
        return -1;
    }
    
    return 0;
}

// Create unified event loop
event_loop_t *event_loop_create(int max_events) {
    event_loop_t *loop = malloc(sizeof(event_loop_t));
    if (!loop) {
        log_error("Failed to allocate event loop memory");
        return NULL;
    }
    
    memset(loop, 0, sizeof(event_loop_t));
    loop->max_events = max_events;
    loop->batch_size = 1000;  // Default batch size
    loop->timeout_ms = 10;    // Default 10ms timeout
    
    // Initialize atomic variables
    atomic_init(&loop->stop, 0);
    atomic_init(&loop->handler_count, 0);
    atomic_init(&loop->active_handlers, 0);
    atomic_init(&loop->total_events_processed, 0);
    atomic_init(&loop->batch_events_processed, 0);
    atomic_init(&loop->error_count, 0);
    atomic_init(&loop->timeout_count, 0);
    atomic_init(&loop->lock_contention, 0);
    
    // Initialize time statistics
    loop->avg_event_processing_time = 0.0;
    loop->max_event_processing_time = 0.0;
    loop->min_event_processing_time = 1e9;
    spinlock_init(&loop->stats_lock);
    
    // Set hash table size (4096 buckets)
    loop->table_size = 4096;
    
    // Initialize main mutex
    if (pthread_mutex_init(&loop->mutex, NULL) != 0) {
        log_error("Failed to initialize mutex");
        free(loop);
        return NULL;
    }
    
    // Initialize read-write lock array
    loop->rwlocks = malloc(sizeof(pthread_rwlock_t) * loop->table_size);
    if (!loop->rwlocks) {
        log_error("Failed to allocate read-write lock array memory");
        pthread_mutex_destroy(&loop->mutex);
        free(loop);
        return NULL;
    }
    
    for (int i = 0; i < loop->table_size; i++) {
        if (pthread_rwlock_init(&loop->rwlocks[i], NULL) != 0) {
            log_error("Failed to initialize read-write lock");
            for (int j = 0; j < i; j++) {
                pthread_rwlock_destroy(&loop->rwlocks[j]);
            }
            free(loop->rwlocks);
            pthread_mutex_destroy(&loop->mutex);
            free(loop);
            return NULL;
        }
    }
    
    // Initialize hash table
    loop->handler_table = calloc(loop->table_size, sizeof(handler_node_t *));
    if (!loop->handler_table) {
        log_error("Failed to allocate hash table memory");
        for (int i = 0; i < loop->table_size; i++) {
            pthread_rwlock_destroy(&loop->rwlocks[i]);
        }
        free(loop->rwlocks);
        pthread_mutex_destroy(&loop->mutex);
        free(loop);
        return NULL;
    }
    
#ifdef __linux__
    // Create epoll instance
    loop->epoll_fd = epoll_create1(0);
    if (loop->epoll_fd == -1) {
        log_error("Failed to create epoll instance: %s", strerror(errno));
        free(loop->handler_table);
        for (int i = 0; i < loop->table_size; i++) {
            pthread_rwlock_destroy(&loop->rwlocks[i]);
        }
        free(loop->rwlocks);
        pthread_mutex_destroy(&loop->mutex);
        free(loop);
        return NULL;
    }
    
    // Allocate events array
    loop->events = malloc(sizeof(struct epoll_event) * max_events);
    if (!loop->events) {
        log_error("Failed to allocate events array memory");
        close(loop->epoll_fd);
        free(loop->handler_table);
        for (int i = 0; i < loop->table_size; i++) {
            pthread_rwlock_destroy(&loop->rwlocks[i]);
        }
        free(loop->rwlocks);
        pthread_mutex_destroy(&loop->mutex);
        free(loop);
        return NULL;
    }
    
    log_info("Unified event loop created successfully (epoll): max_events=%d, batch_size=%d, timeout_ms=%d", 
             max_events, loop->batch_size, loop->timeout_ms);
#else
    // Create kqueue instance
    loop->kqueue_fd = kqueue();
    if (loop->kqueue_fd == -1) {
        log_error("Failed to create kqueue instance: %s", strerror(errno));
        free(loop->handler_table);
        for (int i = 0; i < loop->table_size; i++) {
            pthread_rwlock_destroy(&loop->rwlocks[i]);
        }
        free(loop->rwlocks);
        pthread_mutex_destroy(&loop->mutex);
        free(loop);
        return NULL;
    }
    
    // Allocate events array
    loop->events = malloc(sizeof(struct kevent) * max_events);
    if (!loop->events) {
        log_error("Failed to allocate events array memory");
        close(loop->kqueue_fd);
        free(loop->handler_table);
        for (int i = 0; i < loop->table_size; i++) {
            pthread_rwlock_destroy(&loop->rwlocks[i]);
        }
        free(loop->rwlocks);
        pthread_mutex_destroy(&loop->mutex);
        free(loop);
        return NULL;
    }
    
    log_info("Unified event loop created successfully (kqueue): max_events=%d, batch_size=%d, timeout_ms=%d", 
             max_events, loop->batch_size, loop->timeout_ms);
#endif
    
    return loop;
}

// Destroy unified event loop
void event_loop_destroy(event_loop_t *loop) {
    if (!loop) {
        return;
    }
    
    log_info("Destroying unified event loop");
    
    // Stop event loop
    event_loop_stop(loop);
    
    // 等待事件循环线程结束
    if (loop->thread_id != 0) {
        pthread_join(loop->thread_id, NULL);
    }
    
    // Clean up all handlers in hash table
    for (int i = 0; i < loop->table_size; i++) {
        handler_node_t *current = loop->handler_table[i];
        while (current) {
            handler_node_t *next = current->next;
            if (current->handler) {
                if (atomic_fetch_sub(&current->handler->ref_count, 1) == 1) {
                    free(current->handler);
                }
            }
            free(current);
            current = next;
        }
    }
    
    // Clean up read-write lock array
    if (loop->rwlocks) {
        for (int i = 0; i < loop->table_size; i++) {
            pthread_rwlock_destroy(&loop->rwlocks[i]);
        }
        free(loop->rwlocks);
    }
    
    // Clean up hash table
    if (loop->handler_table) {
        free(loop->handler_table);
    }
    
#ifdef __linux__
    if (loop->epoll_fd != -1) {
        close(loop->epoll_fd);
    }
#else
    if (loop->kqueue_fd != -1) {
        close(loop->kqueue_fd);
    }
#endif
    
    if (loop->events) {
        free(loop->events);
    }
    
    pthread_mutex_destroy(&loop->mutex);
    free(loop);
    
    log_info("Unified event loop destruction completed");
}

// Add event handler
int event_loop_add_handler(event_loop_t *loop, int fd, int events, 
                          event_callback_t read_cb, event_callback_t write_cb, void *arg) {
    if (!loop || fd < 0) {
        return -1;
    }
    
    // Set to non-blocking mode
    if (set_nonblocking(fd) != 0) {
        return -1;
    }
    
    // Create event handler
    event_handler_t *handler = malloc(sizeof(event_handler_t));
    if (!handler) {
        log_error("Failed to allocate event handler memory");
        return -1;
    }
    
    handler->fd = fd;
    handler->read_cb = read_cb;
    handler->write_cb = write_cb;
    handler->arg = arg;
    handler->events = events;
    atomic_init(&handler->ref_count, 1);
    atomic_init(&handler->active, 1);
    handler->processing_count = 0;
    handler->avg_processing_time = 0.0;
    clock_gettime(CLOCK_MONOTONIC, &handler->last_activity);
    
    pthread_mutex_lock(&loop->mutex);
    
    // Add to hash table
    add_handler_to_table(loop, fd, handler);
    
#ifdef __linux__
    // Add to epoll
    struct epoll_event ev;
    ev.events = 0;
    if (events & EVENT_READ) {
        ev.events |= EPOLLIN;
    }
    if (events & EVENT_WRITE) {
        ev.events |= EPOLLOUT;
    }
    ev.events |= EPOLLET; // Edge-triggered mode
    ev.data.ptr = handler;
    
    if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_ADD, fd, &ev) == -1) {
        log_error("Failed to add event handler: %s", strerror(errno));
        remove_handler_from_table(loop, fd);
        pthread_mutex_unlock(&loop->mutex);
        free(handler);
        return -1;
    }
#else
    // Add to kqueue
    struct kevent ev[2];
    int n = 0;
    
    if (events & EVENT_READ) {
        EV_SET(&ev[n++], fd, EVFILT_READ, EV_ADD | EV_ENABLE | EV_CLEAR, 0, 0, handler);
    }
    
    if (events & EVENT_WRITE) {
        EV_SET(&ev[n++], fd, EVFILT_WRITE, EV_ADD | EV_ENABLE | EV_CLEAR, 0, 0, handler);
    }
    
    if (kevent(loop->kqueue_fd, ev, n, NULL, 0, NULL) == -1) {
        log_error("Failed to add event handler: %s", strerror(errno));
        remove_handler_from_table(loop, fd);
        pthread_mutex_unlock(&loop->mutex);
        free(handler);
        return -1;
    }
#endif
    
    pthread_mutex_unlock(&loop->mutex);
    
    log_debug("Event handler added successfully: fd=%d, events=%d", fd, events);
    return 0;
}

// Modify event handler
int event_loop_mod_handler(event_loop_t *loop, int fd, int events, 
                          event_callback_t read_cb, event_callback_t write_cb, void *arg) {
    if (!loop || fd < 0) {
        return -1;
    }
    
    pthread_mutex_lock(&loop->mutex);
    
#ifdef __linux__
    struct epoll_event ev;
    ev.events = 0;
    if (events & EVENT_READ) {
        ev.events |= EPOLLIN;
    }
    if (events & EVENT_WRITE) {
        ev.events |= EPOLLOUT;
    }
    ev.events |= EPOLLET;
    
    // Create new event handler
    event_handler_t *handler = malloc(sizeof(event_handler_t));
    if (!handler) {
        log_error("Failed to allocate event handler memory");
        pthread_mutex_unlock(&loop->mutex);
        return -1;
    }
    
    handler->fd = fd;
    handler->read_cb = read_cb;
    handler->write_cb = write_cb;
    handler->arg = arg;
    handler->events = events;
    atomic_init(&handler->ref_count, 1);
    atomic_init(&handler->active, 1);
    handler->processing_count = 0;
    handler->avg_processing_time = 0.0;
    clock_gettime(CLOCK_MONOTONIC, &handler->last_activity);
    
    ev.data.ptr = handler;
    
    if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_MOD, fd, &ev) == -1) {
        if (errno == ENOENT) {
            if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_ADD, fd, &ev) == -1) {
                log_error("Failed to add event handler: %s", strerror(errno));
                pthread_mutex_unlock(&loop->mutex);
                free(handler);
                return -1;
            }
            add_handler_to_table(loop, fd, handler);
        } else {
            log_error("Failed to modify event handler: %s", strerror(errno));
            pthread_mutex_unlock(&loop->mutex);
            free(handler);
            return -1;
        }
    }
#else
    // kqueue modification logic
    struct kevent ev[4];
    int n = 0;
    
    EV_SET(&ev[n++], fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    EV_SET(&ev[n++], fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    
    event_handler_t *handler = malloc(sizeof(event_handler_t));
    if (!handler) {
        log_error("Failed to allocate event handler memory");
        pthread_mutex_unlock(&loop->mutex);
        return -1;
    }
    
    handler->fd = fd;
    handler->read_cb = read_cb;
    handler->write_cb = write_cb;
    handler->arg = arg;
    handler->events = events;
    atomic_init(&handler->ref_count, 1);
    atomic_init(&handler->active, 1);
    handler->processing_count = 0;
    handler->avg_processing_time = 0.0;
    clock_gettime(CLOCK_MONOTONIC, &handler->last_activity);
    
    if (events & EVENT_READ) {
        EV_SET(&ev[n++], fd, EVFILT_READ, EV_ADD | EV_ENABLE | EV_CLEAR, 0, 0, handler);
    }
    
    if (events & EVENT_WRITE) {
        EV_SET(&ev[n++], fd, EVFILT_WRITE, EV_ADD | EV_ENABLE | EV_CLEAR, 0, 0, handler);
    }
    
    if (kevent(loop->kqueue_fd, ev, n, NULL, 0, NULL) == -1) {
        log_error("Failed to modify event handler: %s", strerror(errno));
        pthread_mutex_unlock(&loop->mutex);
        free(handler);
        return -1;
    }
#endif
    
    pthread_mutex_unlock(&loop->mutex);
    
    log_debug("Event handler modified successfully: fd=%d, events=%d", fd, events);
    return 0;
}

// Delete event handler
int event_loop_del_handler(event_loop_t *loop, int fd) {
    if (!loop || fd < 0) {
        return -1;
    }
    
    pthread_mutex_lock(&loop->mutex);
    
    event_handler_t *handler_to_free = remove_handler_from_table(loop, fd);
    
#ifdef __linux__
    if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_DEL, fd, NULL) == -1) {
        if (errno != ENOENT) {
            log_error("Failed to delete event handler: %s", strerror(errno));
            pthread_mutex_unlock(&loop->mutex);
            return -1;
        }
    }
#else
    struct kevent ev[2];
    EV_SET(&ev[0], fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    EV_SET(&ev[1], fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    kevent(loop->kqueue_fd, ev, 2, NULL, 0, NULL);
#endif
    
    pthread_mutex_unlock(&loop->mutex);
    
    if (handler_to_free) {
        free(handler_to_free);
        log_debug("Event handler deleted successfully: fd=%d", fd);
    }
    
    return 0;
}

// Unified event loop thread function
static void *event_loop_thread(void *arg) {
    event_loop_t *loop = (event_loop_t *)arg;
    log_info("Unified event loop thread started");
    
    // Set thread signal mask
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGQUIT);
    pthread_sigmask(SIG_UNBLOCK, &set, NULL);
    
    while (!atomic_load(&loop->stop)) {
        uint64_t loop_start = get_time_us();
        
#ifdef __linux__
        int nfds = epoll_wait(loop->epoll_fd, loop->events, loop->max_events, loop->timeout_ms);
        
        if (nfds == -1) {
            if (errno == EINTR) {
                continue;
            }
            log_error("epoll_wait failed: %s", strerror(errno));
            atomic_fetch_add(&loop->error_count, 1);
            break;
        }
        
        if (nfds == 0) {
            atomic_fetch_add(&loop->timeout_count, 1);
            continue;
        }
        
        // Batch process events
        for (int i = 0; i < nfds && !atomic_load(&loop->stop); i++) {
            event_handler_t *handler = (event_handler_t *)loop->events[i].data.ptr;
            
            if (!handler || !atomic_load(&handler->active)) {
                continue;
            }
            
            atomic_fetch_add(&handler->ref_count, 1);
            int fd = handler->fd;
            
            // Handle error events
            if (loop->events[i].events & (EPOLLERR | EPOLLHUP)) {
                if (handler->read_cb) {
                    handler->read_cb(fd, handler->arg);
                }
                atomic_fetch_sub(&handler->ref_count, 1);
                continue;
            }
            
            // Handle read events
            if ((loop->events[i].events & EPOLLIN) && handler->read_cb) {
                handler->read_cb(fd, handler->arg);
            }
            
            // Check if handler is still valid
            if (handler->fd != fd) {
                atomic_fetch_sub(&handler->ref_count, 1);
                continue;
            }
            
            // Handle write events
            if ((loop->events[i].events & EPOLLOUT) && handler->write_cb) {
                handler->write_cb(fd, handler->arg);
            }
            
            atomic_fetch_sub(&handler->ref_count, 1);
        }
#else
        struct timespec timeout;
        timeout.tv_sec = loop->timeout_ms / 1000;
        timeout.tv_nsec = (loop->timeout_ms % 1000) * 1000000;
        
        int nfds = kevent(loop->kqueue_fd, NULL, 0, loop->events, loop->max_events, &timeout);
        
        if (nfds == -1) {
            if (errno == EINTR) {
                continue;
            }
            log_error("kevent failed: %s", strerror(errno));
            atomic_fetch_add(&loop->error_count, 1);
            break;
        }
        
        if (nfds == 0) {
            atomic_fetch_add(&loop->timeout_count, 1);
            continue;
        }
        
        // Batch process events
        for (int i = 0; i < nfds && !atomic_load(&loop->stop); i++) {
            event_handler_t *handler = (event_handler_t *)loop->events[i].udata;
            
            if (!handler || !atomic_load(&handler->active)) {
                continue;
            }
            
            atomic_fetch_add(&handler->ref_count, 1);
            int fd = handler->fd;
            
            // Handle read events
            if (loop->events[i].filter == EVFILT_READ && handler->read_cb) {
                handler->read_cb(fd, handler->arg);
            }
            
            // Check if handler is still valid
            if (handler->fd != fd) {
                atomic_fetch_sub(&handler->ref_count, 1);
                continue;
            }
            
            // Handle write events
            if (loop->events[i].filter == EVFILT_WRITE && handler->write_cb) {
                handler->write_cb(fd, handler->arg);
            }
            
            atomic_fetch_sub(&handler->ref_count, 1);
        }
#endif
        
        // Update statistics
        uint64_t loop_end = get_time_us();
        uint64_t processing_time = loop_end - loop_start;
        
        atomic_fetch_add(&loop->total_events_processed, nfds);
        if (nfds > loop->batch_size) {
            atomic_fetch_add(&loop->batch_events_processed, nfds);
        }
        
        // Update time statistics (protected by spinlock)
        spinlock_lock(&loop->stats_lock);
        
        uint64_t total_processed = atomic_load(&loop->total_events_processed);
        if (total_processed > 0) {
            loop->avg_event_processing_time = 
                (loop->avg_event_processing_time * (total_processed - nfds) + processing_time) / total_processed;
        }
        
        if (processing_time > loop->max_event_processing_time) {
            loop->max_event_processing_time = processing_time;
        }
        if (processing_time < loop->min_event_processing_time) {
            loop->min_event_processing_time = processing_time;
        }
        
        spinlock_unlock(&loop->stats_lock);
    }
    
    log_info("Unified event loop thread exited");
    return NULL;
}

// Start unified event loop
int event_loop_start(event_loop_t *loop) {
    if (!loop) {
        return -1;
    }
    
    if (pthread_create(&loop->thread_id, NULL, event_loop_thread, loop) != 0) {
        log_error("Failed to create unified event loop thread");
        return -1;
    }
    
    log_info("Unified event loop started");
    return 0;
}

// Stop unified event loop
void event_loop_stop(event_loop_t *loop) {
    if (!loop) {
        return;
    }
    
    log_info("Stopping unified event loop");
    atomic_store(&loop->stop, 1);
    
    // If event loop thread exists, send signal to interrupt possible blocking calls
    if (loop->thread_id != 0) {
        pthread_kill(loop->thread_id, SIGTERM);
    }
}

// Wait for event loop to end
void event_loop_wait(event_loop_t *loop) {
    if (!loop || loop->thread_id == 0) {
        return;
    }
    
    pthread_join(loop->thread_id, NULL);
    loop->thread_id = 0;
    
    log_info("Unified event loop thread ended");
}

// Check if event loop is stopped
int event_loop_is_stopped(event_loop_t *loop) {
    if (!loop) {
        return 1;
    }
    
    return atomic_load(&loop->stop);
}

// Get event loop statistics
void event_loop_get_stats(event_loop_t *loop, int *handler_count, int *active_handlers) {
    if (!loop) {
        if (handler_count) *handler_count = 0;
        if (active_handlers) *active_handlers = 0;
        return;
    }
    
    if (handler_count) {
        *handler_count = atomic_load(&loop->handler_count);
    }
    if (active_handlers) {
        *active_handlers = atomic_load(&loop->active_handlers);
    }
}

// Get detailed statistics
void event_loop_get_detailed_stats(event_loop_t *loop, event_loop_detailed_stats_t *stats) {
    if (!loop || !stats) {
        return;
    }
    
    memset(stats, 0, sizeof(event_loop_detailed_stats_t));
    
    stats->total_events_processed = atomic_load(&loop->total_events_processed);
    stats->batch_events_processed = atomic_load(&loop->batch_events_processed);
    stats->error_count = atomic_load(&loop->error_count);
    stats->timeout_count = atomic_load(&loop->timeout_count);
    stats->lock_contention = atomic_load(&loop->lock_contention);
    stats->handler_count = atomic_load(&loop->handler_count);
    stats->active_handlers = atomic_load(&loop->active_handlers);
    
    spinlock_lock(&loop->stats_lock);
    stats->avg_event_processing_time = loop->avg_event_processing_time;
    stats->max_event_processing_time = loop->max_event_processing_time;
    stats->min_event_processing_time = loop->min_event_processing_time;
    spinlock_unlock(&loop->stats_lock);
}

// Reset statistics
void event_loop_reset_stats(event_loop_t *loop) {
    if (!loop) {
        return;
    }
    
    atomic_store(&loop->total_events_processed, 0);
    atomic_store(&loop->batch_events_processed, 0);
    atomic_store(&loop->error_count, 0);
    atomic_store(&loop->timeout_count, 0);
    atomic_store(&loop->lock_contention, 0);
    
    spinlock_lock(&loop->stats_lock);
    loop->avg_event_processing_time = 0.0;
    loop->max_event_processing_time = 0.0;
    loop->min_event_processing_time = 1e9;
    spinlock_unlock(&loop->stats_lock);
    
    log_info("Unified event loop statistics reset");
}

// Set batch size
int event_loop_set_batch_size(event_loop_t *loop, int batch_size) {
    if (!loop || batch_size <= 0) {
        return -1;
    }
    
    loop->batch_size = batch_size;
    log_info("Batch size updated to: %d", batch_size);
    return 0;
}

// Set timeout
int event_loop_set_timeout(event_loop_t *loop, int timeout_ms) {
    if (!loop || timeout_ms <= 0) {
        return -1;
    }
    
    loop->timeout_ms = timeout_ms;
    log_info("Timeout updated to: %dms", timeout_ms);
    return 0;
}

// Print statistics
void event_loop_print_stats(event_loop_t *loop) {
    if (!loop) {
        return;
    }
    
    event_loop_detailed_stats_t stats;
    event_loop_get_detailed_stats(loop, &stats);
    
    log_info("=== Unified Event Loop Statistics ===");
    log_info("Total events processed: %lu", stats.total_events_processed);
    log_info("Batch events processed: %lu", stats.batch_events_processed);
    log_info("Average event processing time: %.2f microseconds", stats.avg_event_processing_time);
    log_info("Maximum event processing time: %.2f microseconds", stats.max_event_processing_time);
    log_info("Minimum event processing time: %.2f microseconds", stats.min_event_processing_time);
    log_info("Handler count: %d", stats.handler_count);
    log_info("Active handler count: %d", stats.active_handlers);
    log_info("Error count: %lu", stats.error_count);
    log_info("Timeout count: %lu", stats.timeout_count);
    log_info("Lock contention count: %lu", stats.lock_contention);
    log_info("=====================================");
}

/**
 * 统一高性能事件驱动I/O框架实现
 * 合并原版和增强版的优点，提供最佳性能和稳定性
 * 参考Nginx和epoll/kqueue最佳实践
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

// 高性能自旋锁结构体
typedef struct spinlock {
    atomic_int locked;
    atomic_int contention_count;  // 竞争统计
} spinlock_t;

// 统一事件处理器结构体
struct event_handler {
    int fd;                                    // 文件描述符
    event_callback_t read_cb;                  // 读事件回调
    event_callback_t write_cb;                 // 写事件回调
    void *arg;                                 // 回调参数
    int events;                                // 关注的事件
    atomic_int ref_count;                      // 引用计数
    atomic_int active;                         // 活跃状态
    struct timespec last_activity;             // 最后活动时间
    uint64_t processing_count;                 // 处理次数
    double avg_processing_time;                // 平均处理时间
};

// 哈希表节点
typedef struct handler_node {
    int fd;
    event_handler_t *handler;
    struct handler_node *next;
    atomic_int ref_count;
} handler_node_t;

// 统一事件循环结构体
struct event_loop {
#ifdef __linux__
    int epoll_fd;                              // epoll文件描述符
    struct epoll_event *events;                // epoll事件数组
#else
    int kqueue_fd;                             // kqueue文件描述符
    struct kevent *events;                     // kqueue事件数组
#endif
    int max_events;                            // 最大事件数
    int batch_size;                            // 批处理大小
    int timeout_ms;                            // 超时时间（毫秒）
    atomic_int stop;                           // 停止标志
    pthread_t thread_id;                       // 事件循环线程ID
    
    // 哈希表管理（高性能查找）
    handler_node_t **handler_table;            // 事件处理器哈希表
    int table_size;                            // 哈希表大小
    pthread_rwlock_t *rwlocks;                 // 分段读写锁
    atomic_int handler_count;                  // 处理器数量
    atomic_int active_handlers;                // 活跃处理器数量
    
    // 性能统计
    _Atomic uint64_t total_events_processed;    // 总处理事件数
    _Atomic uint64_t batch_events_processed;    // 批处理事件数
    _Atomic uint64_t error_count;               // 错误次数
    _Atomic uint64_t timeout_count;             // 超时次数
    _Atomic uint64_t lock_contention;           // 锁竞争统计
    
    // 时间统计
    double avg_event_processing_time;          // 平均事件处理时间
    double max_event_processing_time;          // 最大事件处理时间
    double min_event_processing_time;          // 最小事件处理时间
    spinlock_t stats_lock;                     // 统计锁
    
    // 主锁
    pthread_mutex_t mutex;                     // 主互斥锁
};

// 自旋锁操作（优化版）
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
            // 自旋过多，让出CPU
            sched_yield();
            spin_count = 0;
            atomic_fetch_add(&lock->contention_count, 1);
        } else {
            // 短暂自旋 - 跨平台兼容
#ifdef __x86_64__
            __asm__ volatile("pause");
#elif defined(__aarch64__) || defined(__arm64__)
            __asm__ volatile("yield");
#else
            // 通用平台使用简单的延迟
            for (volatile int i = 0; i < 10; i++) {
                // 空循环作为延迟
            }
#endif
        }
    }
}

static inline void spinlock_unlock(spinlock_t *lock) {
    atomic_store(&lock->locked, 0);
}

// 获取当前时间（微秒）
static inline uint64_t get_time_us() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
}

// 优化的哈希函数
static unsigned int hash_fd(int fd) {
    unsigned int hash = (unsigned int)fd;
    hash = ((hash << 13) ^ hash) ^ (hash >> 17);
    hash = ((hash << 5) ^ hash) ^ (hash >> 3);
    return hash;
}

// 获取哈希表索引
static unsigned int get_table_index(event_loop_t *loop, int fd) {
    return hash_fd(fd) % loop->table_size;
}

// 添加处理器到哈希表
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

// 从哈希表删除处理器
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

// 设置文件描述符为非阻塞模式
static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        log_error("获取文件描述符标志失败: %s", strerror(errno));
        return -1;
    }
    
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        log_error("Set non-blocking mode失败: %s", strerror(errno));
        return -1;
    }
    
    return 0;
}

// 创建统一事件循环
event_loop_t *event_loop_create(int max_events) {
    event_loop_t *loop = malloc(sizeof(event_loop_t));
    if (!loop) {
        log_error("无法分配事件循环内存");
        return NULL;
    }
    
    memset(loop, 0, sizeof(event_loop_t));
    loop->max_events = max_events;
    loop->batch_size = 1000;  // 默认批处理大小
    loop->timeout_ms = 10;    // 默认10ms超时
    
    // 初始化原子变量
    atomic_init(&loop->stop, 0);
    atomic_init(&loop->handler_count, 0);
    atomic_init(&loop->active_handlers, 0);
    atomic_init(&loop->total_events_processed, 0);
    atomic_init(&loop->batch_events_processed, 0);
    atomic_init(&loop->error_count, 0);
    atomic_init(&loop->timeout_count, 0);
    atomic_init(&loop->lock_contention, 0);
    
    // 初始化时间统计
    loop->avg_event_processing_time = 0.0;
    loop->max_event_processing_time = 0.0;
    loop->min_event_processing_time = 1e9;
    spinlock_init(&loop->stats_lock);
    
    // 设置哈希表大小（4096个桶）
    loop->table_size = 4096;
    
    // 初始化主互斥锁
    if (pthread_mutex_init(&loop->mutex, NULL) != 0) {
        log_error("初始化互斥锁失败");
        free(loop);
        return NULL;
    }
    
    // 初始化读写锁数组
    loop->rwlocks = malloc(sizeof(pthread_rwlock_t) * loop->table_size);
    if (!loop->rwlocks) {
        log_error("无法分配读写锁数组内存");
        pthread_mutex_destroy(&loop->mutex);
        free(loop);
        return NULL;
    }
    
    for (int i = 0; i < loop->table_size; i++) {
        if (pthread_rwlock_init(&loop->rwlocks[i], NULL) != 0) {
            log_error("初始化读写锁失败");
            for (int j = 0; j < i; j++) {
                pthread_rwlock_destroy(&loop->rwlocks[j]);
            }
            free(loop->rwlocks);
            pthread_mutex_destroy(&loop->mutex);
            free(loop);
            return NULL;
        }
    }
    
    // 初始化哈希表
    loop->handler_table = calloc(loop->table_size, sizeof(handler_node_t *));
    if (!loop->handler_table) {
        log_error("无法分配哈希表内存");
        for (int i = 0; i < loop->table_size; i++) {
            pthread_rwlock_destroy(&loop->rwlocks[i]);
        }
        free(loop->rwlocks);
        pthread_mutex_destroy(&loop->mutex);
        free(loop);
        return NULL;
    }
    
#ifdef __linux__
    // 创建epoll实例
    loop->epoll_fd = epoll_create1(0);
    if (loop->epoll_fd == -1) {
        log_error("创建epoll实例失败: %s", strerror(errno));
        free(loop->handler_table);
        for (int i = 0; i < loop->table_size; i++) {
            pthread_rwlock_destroy(&loop->rwlocks[i]);
        }
        free(loop->rwlocks);
        pthread_mutex_destroy(&loop->mutex);
        free(loop);
        return NULL;
    }
    
    // 分配事件数组
    loop->events = malloc(sizeof(struct epoll_event) * max_events);
    if (!loop->events) {
        log_error("无法分配事件数组内存");
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
    
    log_info("统一事件循环创建成功 (epoll): max_events=%d, batch_size=%d, timeout_ms=%d", 
             max_events, loop->batch_size, loop->timeout_ms);
#else
    // 创建kqueue实例
    loop->kqueue_fd = kqueue();
    if (loop->kqueue_fd == -1) {
        log_error("创建kqueue实例失败: %s", strerror(errno));
        free(loop->handler_table);
        for (int i = 0; i < loop->table_size; i++) {
            pthread_rwlock_destroy(&loop->rwlocks[i]);
        }
        free(loop->rwlocks);
        pthread_mutex_destroy(&loop->mutex);
        free(loop);
        return NULL;
    }
    
    // 分配事件数组
    loop->events = malloc(sizeof(struct kevent) * max_events);
    if (!loop->events) {
        log_error("无法分配事件数组内存");
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
    
    log_info("统一事件循环创建成功 (kqueue): max_events=%d, batch_size=%d, timeout_ms=%d", 
             max_events, loop->batch_size, loop->timeout_ms);
#endif
    
    return loop;
}

// 销毁统一事件循环
void event_loop_destroy(event_loop_t *loop) {
    if (!loop) {
        return;
    }
    
    log_info("正在销毁统一事件循环");
    
    // Stop event loop
    event_loop_stop(loop);
    
    // 等待事件循环线程结束
    if (loop->thread_id != 0) {
        pthread_join(loop->thread_id, NULL);
    }
    
    // 清理哈希表中的所有处理器
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
    
    // 清理读写锁数组
    if (loop->rwlocks) {
        for (int i = 0; i < loop->table_size; i++) {
            pthread_rwlock_destroy(&loop->rwlocks[i]);
        }
        free(loop->rwlocks);
    }
    
    // 清理哈希表
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
    
    log_info("统一事件循环销毁完成");
}

// 添加事件处理器
int event_loop_add_handler(event_loop_t *loop, int fd, int events, 
                          event_callback_t read_cb, event_callback_t write_cb, void *arg) {
    if (!loop || fd < 0) {
        return -1;
    }
    
    // 设置为非阻塞模式
    if (set_nonblocking(fd) != 0) {
        return -1;
    }
    
    // 创建事件处理器
    event_handler_t *handler = malloc(sizeof(event_handler_t));
    if (!handler) {
        log_error("无法分配事件处理器内存");
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
    
    // 添加到哈希表
    add_handler_to_table(loop, fd, handler);
    
#ifdef __linux__
    // 添加到epoll
    struct epoll_event ev;
    ev.events = 0;
    if (events & EVENT_READ) {
        ev.events |= EPOLLIN;
    }
    if (events & EVENT_WRITE) {
        ev.events |= EPOLLOUT;
    }
    ev.events |= EPOLLET; // 边缘触发模式
    ev.data.ptr = handler;
    
    if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_ADD, fd, &ev) == -1) {
        log_error("添加事件处理器失败: %s", strerror(errno));
        remove_handler_from_table(loop, fd);
        pthread_mutex_unlock(&loop->mutex);
        free(handler);
        return -1;
    }
#else
    // 添加到kqueue
    struct kevent ev[2];
    int n = 0;
    
    if (events & EVENT_READ) {
        EV_SET(&ev[n++], fd, EVFILT_READ, EV_ADD | EV_ENABLE | EV_CLEAR, 0, 0, handler);
    }
    
    if (events & EVENT_WRITE) {
        EV_SET(&ev[n++], fd, EVFILT_WRITE, EV_ADD | EV_ENABLE | EV_CLEAR, 0, 0, handler);
    }
    
    if (kevent(loop->kqueue_fd, ev, n, NULL, 0, NULL) == -1) {
        log_error("添加事件处理器失败: %s", strerror(errno));
        remove_handler_from_table(loop, fd);
        pthread_mutex_unlock(&loop->mutex);
        free(handler);
        return -1;
    }
#endif
    
    pthread_mutex_unlock(&loop->mutex);
    
    log_debug("添加事件处理器成功: fd=%d, events=%d", fd, events);
    return 0;
}

// 修改事件处理器
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
    
    // 创建新的事件处理器
    event_handler_t *handler = malloc(sizeof(event_handler_t));
    if (!handler) {
        log_error("无法分配事件处理器内存");
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
                log_error("添加事件处理器失败: %s", strerror(errno));
                pthread_mutex_unlock(&loop->mutex);
                free(handler);
                return -1;
            }
            add_handler_to_table(loop, fd, handler);
        } else {
            log_error("修改事件处理器失败: %s", strerror(errno));
            pthread_mutex_unlock(&loop->mutex);
            free(handler);
            return -1;
        }
    }
#else
    // kqueue修改逻辑
    struct kevent ev[4];
    int n = 0;
    
    EV_SET(&ev[n++], fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    EV_SET(&ev[n++], fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
    
    event_handler_t *handler = malloc(sizeof(event_handler_t));
    if (!handler) {
        log_error("无法分配事件处理器内存");
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
        log_error("修改事件处理器失败: %s", strerror(errno));
        pthread_mutex_unlock(&loop->mutex);
        free(handler);
        return -1;
    }
#endif
    
    pthread_mutex_unlock(&loop->mutex);
    
    log_debug("修改事件处理器成功: fd=%d, events=%d", fd, events);
    return 0;
}

// 删除事件处理器
int event_loop_del_handler(event_loop_t *loop, int fd) {
    if (!loop || fd < 0) {
        return -1;
    }
    
    pthread_mutex_lock(&loop->mutex);
    
    event_handler_t *handler_to_free = remove_handler_from_table(loop, fd);
    
#ifdef __linux__
    if (epoll_ctl(loop->epoll_fd, EPOLL_CTL_DEL, fd, NULL) == -1) {
        if (errno != ENOENT) {
            log_error("删除事件处理器失败: %s", strerror(errno));
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
        log_debug("删除事件处理器成功: fd=%d", fd);
    }
    
    return 0;
}

// 统一事件循环线程函数
static void *event_loop_thread(void *arg) {
    event_loop_t *loop = (event_loop_t *)arg;
    log_info("统一事件循环线程启动");
    
    // 设置线程信号掩码
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
            log_error("epoll_wait失败: %s", strerror(errno));
            atomic_fetch_add(&loop->error_count, 1);
            break;
        }
        
        if (nfds == 0) {
            atomic_fetch_add(&loop->timeout_count, 1);
            continue;
        }
        
        // 批处理事件
        for (int i = 0; i < nfds && !atomic_load(&loop->stop); i++) {
            event_handler_t *handler = (event_handler_t *)loop->events[i].data.ptr;
            
            if (!handler || !atomic_load(&handler->active)) {
                continue;
            }
            
            atomic_fetch_add(&handler->ref_count, 1);
            int fd = handler->fd;
            
            // 处理错误事件
            if (loop->events[i].events & (EPOLLERR | EPOLLHUP)) {
                if (handler->read_cb) {
                    handler->read_cb(fd, handler->arg);
                }
                atomic_fetch_sub(&handler->ref_count, 1);
                continue;
            }
            
            // 处理读事件
            if ((loop->events[i].events & EPOLLIN) && handler->read_cb) {
                handler->read_cb(fd, handler->arg);
            }
            
            // 检查handler是否仍然有效
            if (handler->fd != fd) {
                atomic_fetch_sub(&handler->ref_count, 1);
                continue;
            }
            
            // 处理写事件
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
            log_error("kevent失败: %s", strerror(errno));
            atomic_fetch_add(&loop->error_count, 1);
            break;
        }
        
        if (nfds == 0) {
            atomic_fetch_add(&loop->timeout_count, 1);
            continue;
        }
        
        // 批处理事件
        for (int i = 0; i < nfds && !atomic_load(&loop->stop); i++) {
            event_handler_t *handler = (event_handler_t *)loop->events[i].udata;
            
            if (!handler || !atomic_load(&handler->active)) {
                continue;
            }
            
            atomic_fetch_add(&handler->ref_count, 1);
            int fd = handler->fd;
            
            // 处理读事件
            if (loop->events[i].filter == EVFILT_READ && handler->read_cb) {
                handler->read_cb(fd, handler->arg);
            }
            
            // 检查handler是否仍然有效
            if (handler->fd != fd) {
                atomic_fetch_sub(&handler->ref_count, 1);
                continue;
            }
            
            // 处理写事件
            if (loop->events[i].filter == EVFILT_WRITE && handler->write_cb) {
                handler->write_cb(fd, handler->arg);
            }
            
            atomic_fetch_sub(&handler->ref_count, 1);
        }
#endif
        
        // 更新统计信息
        uint64_t loop_end = get_time_us();
        uint64_t processing_time = loop_end - loop_start;
        
        atomic_fetch_add(&loop->total_events_processed, nfds);
        if (nfds > loop->batch_size) {
            atomic_fetch_add(&loop->batch_events_processed, nfds);
        }
        
        // 更新时间统计（使用自旋锁保护）
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
    
    log_info("统一事件循环线程退出");
    return NULL;
}

// 启动统一事件循环
int event_loop_start(event_loop_t *loop) {
    if (!loop) {
        return -1;
    }
    
    if (pthread_create(&loop->thread_id, NULL, event_loop_thread, loop) != 0) {
        log_error("创建统一事件循环线程失败");
        return -1;
    }
    
    log_info("统一事件循环已启动");
    return 0;
}

// 停止统一事件循环
void event_loop_stop(event_loop_t *loop) {
    if (!loop) {
        return;
    }
    
    log_info("正在停止统一事件循环");
    atomic_store(&loop->stop, 1);
    
    // 如果事件循环线程存在，向其发送信号以中断可能的阻塞调用
    if (loop->thread_id != 0) {
        pthread_kill(loop->thread_id, SIGTERM);
    }
}

// 等待事件循环结束
void event_loop_wait(event_loop_t *loop) {
    if (!loop || loop->thread_id == 0) {
        return;
    }
    
    pthread_join(loop->thread_id, NULL);
    loop->thread_id = 0;
    
    log_info("统一事件循环线程已结束");
}

// 检查事件循环是否已停止
int event_loop_is_stopped(event_loop_t *loop) {
    if (!loop) {
        return 1;
    }
    
    return atomic_load(&loop->stop);
}

// 获取事件循环统计信息
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

// 获取详细统计信息
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

// 重置统计信息
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
    
    log_info("统一事件循环统计信息已重置");
}

// 设置批处理大小
int event_loop_set_batch_size(event_loop_t *loop, int batch_size) {
    if (!loop || batch_size <= 0) {
        return -1;
    }
    
    loop->batch_size = batch_size;
    log_info("批处理大小已更新为: %d", batch_size);
    return 0;
}

// 设置超时时间
int event_loop_set_timeout(event_loop_t *loop, int timeout_ms) {
    if (!loop || timeout_ms <= 0) {
        return -1;
    }
    
    loop->timeout_ms = timeout_ms;
    log_info("超时时间已更新为: %dms", timeout_ms);
    return 0;
}

// 打印统计信息
void event_loop_print_stats(event_loop_t *loop) {
    if (!loop) {
        return;
    }
    
    event_loop_detailed_stats_t stats;
    event_loop_get_detailed_stats(loop, &stats);
    
    log_info("=== 统一事件循环统计信息 ===");
    log_info("总处理事件数: %lu", stats.total_events_processed);
    log_info("批处理事件数: %lu", stats.batch_events_processed);
    log_info("平均事件处理时间: %.2f 微秒", stats.avg_event_processing_time);
    log_info("最大事件处理时间: %.2f 微秒", stats.max_event_processing_time);
    log_info("最小事件处理时间: %.2f 微秒", stats.min_event_processing_time);
    log_info("处理器数量: %d", stats.handler_count);
    log_info("活跃处理器数量: %d", stats.active_handlers);
    log_info("错误次数: %lu", stats.error_count);
    log_info("超时次数: %lu", stats.timeout_count);
    log_info("锁竞争次数: %lu", stats.lock_contention);
    log_info("================================");
}

/**
 * 线程池实现模块
 */

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include "../include/thread_pool.h"
#include "../include/logger.h"

// 任务结构体
typedef struct {
    void (*function)(void *);  // 任务函数
    void *argument;            // 任务参数
} thread_pool_task_t;

// 线程池结构体
struct thread_pool {
    pthread_mutex_t lock;           // 互斥锁
    pthread_cond_t notify;          // 条件变量
    pthread_t *threads;             // 工作线程数组
    thread_pool_task_t *queue;      // 任务队列
    int thread_count;               // 线程数量
    int queue_size;                 // 队列大小
    int head;                       // 队列头
    int tail;                       // 队列尾
    int count;                      // 当前任务数量
    int shutdown;                   // 关闭标志
    int started;                    // 已启动的线程数
};

// 线程工作函数
static void *thread_worker(void *arg) {
    thread_pool_t *pool = (thread_pool_t *)arg;
    thread_pool_task_t task;

    while (1) {
        // 获取任务
        pthread_mutex_lock(&(pool->lock));

        // 等待任务或关闭信号
        while ((pool->count == 0) && (!pool->shutdown)) {
            pthread_cond_wait(&(pool->notify), &(pool->lock));
        }

        // 检查是否需要关闭线程
        if ((pool->shutdown == 1) && (pool->count == 0)) {
            break;
        }

        // 获取任务
        task.function = pool->queue[pool->head].function;
        task.argument = pool->queue[pool->head].argument;
        pool->head = (pool->head + 1) % pool->queue_size;
        pool->count--;

        pthread_mutex_unlock(&(pool->lock));

        // 执行任务
        (*(task.function))(task.argument);
    }

    // 线程退出
    pool->started--;
    pthread_mutex_unlock(&(pool->lock));
    pthread_exit(NULL);
    return NULL;
}

// 创建线程池
thread_pool_t *thread_pool_create(int thread_count, int queue_size) {
    thread_pool_t *pool;
    int i;

    // 检查参数
    if (thread_count <= 0 || queue_size <= 0) {
        return NULL;
    }

    // 分配内存
    pool = (thread_pool_t *)malloc(sizeof(thread_pool_t));
    if (pool == NULL) {
        log_error("无法分配线程池内存");
        return NULL;
    }

    // 初始化线程池
    pool->thread_count = thread_count;
    pool->queue_size = queue_size;
    pool->head = pool->tail = pool->count = 0;
    pool->shutdown = pool->started = 0;

    // 分配线程和任务队列内存
    pool->threads = (pthread_t *)malloc(sizeof(pthread_t) * thread_count);
    pool->queue = (thread_pool_task_t *)malloc(sizeof(thread_pool_task_t) * queue_size);

    // 检查内存分配
    if (pool->threads == NULL || pool->queue == NULL) {
        log_error("无法分配线程池队列内存");
        if (pool->threads) free(pool->threads);
        if (pool->queue) free(pool->queue);
        free(pool);
        return NULL;
    }

    // 初始化互斥锁和条件变量
    if (pthread_mutex_init(&(pool->lock), NULL) != 0 ||
        pthread_cond_init(&(pool->notify), NULL) != 0) {
        log_error("无法初始化线程池锁或条件变量");
        free(pool->threads);
        free(pool->queue);
        free(pool);
        return NULL;
    }

    // 创建工作线程
    for (i = 0; i < thread_count; i++) {
        if (pthread_create(&(pool->threads[i]), NULL, thread_worker, (void*)pool) != 0) {
            thread_pool_destroy(pool, 0);
            return NULL;
        }
        pool->started++;
        log_debug("线程池创建工作线程 %d", i);
    }

    log_info("线程池初始化成功，线程数: %d，队列大小: %d", thread_count, queue_size);
    return pool;
}

// 添加任务到线程池
int thread_pool_add(thread_pool_t *pool, void (*function)(void *), void *argument) {
    int err = 0;
    int next;

    if (pool == NULL || function == NULL) {
        return THREAD_POOL_INVALID;
    }

    // 获取锁
    if (pthread_mutex_lock(&(pool->lock)) != 0) {
        return THREAD_POOL_LOCK_FAILURE;
    }

    // 计算下一个可用位置
    next = (pool->tail + 1) % pool->queue_size;

    // 检查队列是否已满
    if (pool->count == pool->queue_size) {
        err = THREAD_POOL_QUEUE_FULL;
        log_warn("线程池队列已满");
    }
    // 检查线程池是否已关闭
    else if (pool->shutdown) {
        err = THREAD_POOL_SHUTDOWN;
    }
    // 添加任务到队列
    else {
        pool->queue[pool->tail].function = function;
        pool->queue[pool->tail].argument = argument;
        pool->tail = next;
        pool->count++;

        // 通知工作线程
        if (pthread_cond_signal(&(pool->notify)) != 0) {
            err = THREAD_POOL_LOCK_FAILURE;
        }
    }

    // 释放锁
    if (pthread_mutex_unlock(&(pool->lock)) != 0) {
        err = THREAD_POOL_LOCK_FAILURE;
    }

    return err;
}

// 销毁线程池
int thread_pool_destroy(thread_pool_t *pool, int flags) {
    int i, err = 0;

    if (pool == NULL) {
        return THREAD_POOL_INVALID;
    }

    // 获取锁
    if (pthread_mutex_lock(&(pool->lock)) != 0) {
        return THREAD_POOL_LOCK_FAILURE;
    }

    // 检查线程池是否已关闭
    if (pool->shutdown) {
        err = THREAD_POOL_SHUTDOWN;
    }
    else {
        // 设置关闭标志
        pool->shutdown = 1;

        // 唤醒所有工作线程
        if ((pthread_cond_broadcast(&(pool->notify)) != 0) ||
            (pthread_mutex_unlock(&(pool->lock)) != 0)) {
            err = THREAD_POOL_LOCK_FAILURE;
            return err;
        }

        // 等待所有线程完成
        for (i = 0; i < pool->thread_count; i++) {
            if (pthread_join(pool->threads[i], NULL) != 0) {
                err = THREAD_POOL_THREAD_FAILURE;
            }
        }
    }

    // 释放资源
    if (!err || flags) {
        pthread_mutex_destroy(&(pool->lock));
        pthread_cond_destroy(&(pool->notify));
        free(pool->threads);
        free(pool->queue);
        free(pool);
    }

    return err;
}
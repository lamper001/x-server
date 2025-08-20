/**
 * 线程池头文件
 */

#ifndef THREAD_POOL_H
#define THREAD_POOL_H

// 线程池错误码
typedef enum {
    THREAD_POOL_SUCCESS = 0,       // 成功
    THREAD_POOL_INVALID,           // 无效参数
    THREAD_POOL_LOCK_FAILURE,      // 锁操作失败
    THREAD_POOL_QUEUE_FULL,        // 队列已满
    THREAD_POOL_SHUTDOWN,          // 线程池已关闭
    THREAD_POOL_THREAD_FAILURE     // 线程操作失败
} thread_pool_error_t;

// 线程池结构体（不透明类型）
typedef struct thread_pool thread_pool_t;

/**
 * 创建线程池
 * 
 * @param thread_count 线程数量
 * @param queue_size 任务队列大小
 * @return 线程池指针，失败返回NULL
 */
thread_pool_t *thread_pool_create(int thread_count, int queue_size);

/**
 * 添加任务到线程池
 * 
 * @param pool 线程池指针
 * @param function 任务函数
 * @param argument 任务参数
 * @return 成功返回0，失败返回错误码
 */
int thread_pool_add(thread_pool_t *pool, void (*function)(void *), void *argument);

/**
 * 销毁线程池
 * 
 * @param pool 线程池指针
 * @param flags 标志位，0表示等待所有任务完成，非0表示立即关闭
 * @return 成功返回0，失败返回错误码
 */
int thread_pool_destroy(thread_pool_t *pool, int flags);

#endif /* THREAD_POOL_H */
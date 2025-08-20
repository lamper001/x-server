/**
 * 内存池管理模块 - 高性能版本
 * 用于提高内存分配效率和减少内存碎片
 */

#ifndef MEMORY_POOL_H
#define MEMORY_POOL_H

#include <stddef.h>
#include <pthread.h>
#include <stdatomic.h>

// 内存块结构
typedef struct memory_block {
    void *data;
    size_t size;
    int in_use;
    struct memory_block *next;
} memory_block_t;

// 内存池结构（不透明类型，具体实现在.c文件中）
typedef struct memory_pool memory_pool_t;

/**
 * 创建内存池
 * 
 * @param initial_size 初始大小
 * @return 内存池指针，失败返回NULL
 */
memory_pool_t *create_memory_pool(size_t initial_size);

/**
 * 从内存池分配内存
 * 
 * @param pool 内存池
 * @param size 请求大小
 * @return 内存指针，失败返回NULL
 */
void *pool_malloc(memory_pool_t *pool, size_t size);

/**
 * 释放内存到内存池
 * 
 * @param pool 内存池
 * @param ptr 内存指针
 */
void pool_free(memory_pool_t *pool, void *ptr);

/**
 * 销毁内存池
 * 
 * @param pool 内存池
 */
void destroy_memory_pool(memory_pool_t *pool);

/**
 * 获取内存池统计信息
 * 
 * @param pool 内存池
 * @param total_size 总大小
 * @param used_size 已使用大小
 */
void get_pool_stats(memory_pool_t *pool, size_t *total_size, size_t *used_size);

/**
 * 压缩内存池，释放未使用的内存块
 * 
 * @param pool 内存池
 * @return 释放的内存块数量，失败返回-1
 */
int compress_memory_pool(memory_pool_t *pool);

#endif /* MEMORY_POOL_H */

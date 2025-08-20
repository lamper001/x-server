/**
 * 统一高性能事件驱动I/O框架头文件
 * 合并原版和增强版的优点，提供最佳性能和稳定性
 */

#ifndef EVENT_LOOP_H
#define EVENT_LOOP_H

#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

// 事件类型定义
#define EVENT_READ  0x01
#define EVENT_WRITE 0x02

// 前向声明
typedef struct event_loop event_loop_t;
typedef struct event_handler event_handler_t;

// 事件回调函数类型
typedef void (*event_callback_t)(int fd, void *arg);

// 详细统计信息结构体
typedef struct {
    uint64_t total_events_processed;    // 总处理事件数
    uint64_t batch_events_processed;    // 批处理事件数
    uint64_t error_count;               // 错误次数
    uint64_t timeout_count;             // 超时次数
    uint64_t lock_contention;           // 锁竞争统计
    int handler_count;                  // 处理器数量
    int active_handlers;                // 活跃处理器数量
    double avg_event_processing_time;   // 平均事件处理时间
    double max_event_processing_time;   // 最大事件处理时间
    double min_event_processing_time;   // 最小事件处理时间
} event_loop_detailed_stats_t;

// 创建事件循环
event_loop_t *event_loop_create(int max_events);

// 销毁事件循环
void event_loop_destroy(event_loop_t *loop);

// 添加事件处理器
int event_loop_add_handler(event_loop_t *loop, int fd, int events, 
                          event_callback_t read_cb, event_callback_t write_cb, void *arg);

// 修改事件处理器
int event_loop_mod_handler(event_loop_t *loop, int fd, int events, 
                          event_callback_t read_cb, event_callback_t write_cb, void *arg);

// 删除事件处理器
int event_loop_del_handler(event_loop_t *loop, int fd);

// 启动事件循环
int event_loop_start(event_loop_t *loop);

// 停止事件循环
void event_loop_stop(event_loop_t *loop);

// 等待事件循环结束
void event_loop_wait(event_loop_t *loop);

// 检查事件循环是否已停止
int event_loop_is_stopped(event_loop_t *loop);

// 获取基本统计信息
void event_loop_get_stats(event_loop_t *loop, int *handler_count, int *active_handlers);

// 获取详细统计信息
void event_loop_get_detailed_stats(event_loop_t *loop, event_loop_detailed_stats_t *stats);

// 重置统计信息
void event_loop_reset_stats(event_loop_t *loop);

// 设置批处理大小
int event_loop_set_batch_size(event_loop_t *loop, int batch_size);

// 设置超时时间
int event_loop_set_timeout(event_loop_t *loop, int timeout_ms);

// 打印统计信息
void event_loop_print_stats(event_loop_t *loop);

#ifdef __cplusplus
}
#endif

#endif // EVENT_LOOP_H
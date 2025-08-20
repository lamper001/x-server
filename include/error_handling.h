/**
 * 统一错误处理模块
 */

#ifndef ERROR_HANDLING_H
#define ERROR_HANDLING_H

#include <errno.h>
#include <string.h>

// 错误类型定义
typedef enum {
    ERROR_NONE = 0,
    ERROR_MEMORY_ALLOCATION,
    ERROR_NETWORK_CONNECTION,
    ERROR_FILE_OPERATION,
    ERROR_CONFIGURATION,
    ERROR_AUTHENTICATION,
    ERROR_PROXY_UPSTREAM,
    ERROR_BUFFER_OVERFLOW,
    ERROR_TIMEOUT,
    ERROR_INVALID_REQUEST,
    ERROR_PERMISSION_DENIED
} error_type_t;

// 错误信息结构
typedef struct {
    error_type_t type;
    int code;
    char message[256];
    char details[512];
} error_info_t;

// 错误处理宏
#define HANDLE_ERROR(type, code, msg) \
    do { \
        log_error("[%s:%d] %s (errno: %d, %s)", \
                  __FILE__, __LINE__, msg, errno, strerror(errno)); \
        return handle_error_internal(type, code, msg); \
    } while(0)

#define CHECK_NULL_RETURN(ptr, ret_val) \
    do { \
        if ((ptr) == NULL) { \
            log_error("空指针检查失败: %s:%d", __FILE__, __LINE__); \
            return (ret_val); \
        } \
    } while(0)

#define CHECK_BOUNDS(value, min, max, ret_val) \
    do { \
        if ((value) < (min) || (value) > (max)) { \
            log_error("边界检查失败: %s:%d, 值=%d, 范围=[%d,%d]", \
                      __FILE__, __LINE__, (int)(value), (int)(min), (int)(max)); \
            return (ret_val); \
        } \
    } while(0)

// 函数声明
int handle_error_internal(error_type_t type, int code, const char *message);
const char *get_error_string(error_type_t type);
void log_error_with_context(const char *function, int line, const char *format, ...);

#endif // ERROR_HANDLING_H
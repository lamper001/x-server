/**
 * 进程标题设置模块
 * 参考nginx实现，用于设置进程标题显示
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>

#include "../include/process_title.h"

// 保存原始的argv和environ
static char **g_os_argv = NULL;
static char *g_os_argv_last = NULL;

/**
 * Initialize process title setting
 */
int init_process_title(int argc, char **argv, char **envp) {
    int i = 0;
    
    // 保存原始argv
    g_os_argv = argv;
    
    // 找到最后一个环境变量的结束位置
    if (envp[0]) {
        // 计算环境变量数量
        for (i = 0; envp[i]; i++) {
            // 计算环境变量占用的内存大小
        }
        g_os_argv_last = envp[i - 1] + strlen(envp[i - 1]) + 1;
    } else {
        g_os_argv_last = argv[argc - 1] + strlen(argv[argc - 1]) + 1;
    }
    
    return 0;
}

/**
 * 设置进程标题
 */
void setproctitle(const char *fmt, ...) {
    va_list args;
    char title[256];
    
    if (g_os_argv == NULL) {
        return;
    }
    
    // 格式化标题字符串
    va_start(args, fmt);
    vsnprintf(title, sizeof(title), fmt, args);
    va_end(args);
    
    // 清空原有的argv内存
    memset(g_os_argv[0], 0, g_os_argv_last - g_os_argv[0]);
    
    // 设置新的进程标题
    strncpy(g_os_argv[0], title, g_os_argv_last - g_os_argv[0] - 1);
    g_os_argv[1] = NULL;
}
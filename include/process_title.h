/**
 * 进程标题设置模块头文件
 */

#ifndef PROCESS_TITLE_H
#define PROCESS_TITLE_H

/**
 * 初始化进程标题设置
 * @param argc 命令行参数个数
 * @param argv 命令行参数数组
 * @param envp 环境变量数组
 * @return 成功返回0，失败返回-1
 */
int init_process_title(int argc, char **argv, char **envp);

/**
 * 设置进程标题
 * @param fmt 格式化字符串
 * @param ... 可变参数
 */
void setproctitle(const char *fmt, ...);

#endif // PROCESS_TITLE_H
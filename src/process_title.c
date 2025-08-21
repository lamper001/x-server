/**
 * Process Title Setting Module
 * Reference nginx implementation, used for setting process title display
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>

#include "../include/process_title.h"

// Save original argv and environ
static char **g_os_argv = NULL;
static char *g_os_argv_last = NULL;

/**
 * Initialize process title setting
 */
int init_process_title(int argc, char **argv, char **envp) {
    int i = 0;
    
    // Save original argv
    g_os_argv = argv;
    
    // Find the end position of the last environment variable
    if (envp[0]) {
        // Calculate the number of environment variables
        for (i = 0; envp[i]; i++) {
            // Calculate the memory size occupied by environment variables
        }
        g_os_argv_last = envp[i - 1] + strlen(envp[i - 1]) + 1;
    } else {
        g_os_argv_last = argv[argc - 1] + strlen(argv[argc - 1]) + 1;
    }
    
    return 0;
}

/**
 * Set process title
 */
void setproctitle(const char *fmt, ...) {
    va_list args;
    char title[256];
    
    if (g_os_argv == NULL) {
        return;
    }
    
    // Format title string
    va_start(args, fmt);
    vsnprintf(title, sizeof(title), fmt, args);
    va_end(args);
    
    // Clear original argv memory
    memset(g_os_argv[0], 0, g_os_argv_last - g_os_argv[0]);
    
    // Set new process title
    strncpy(g_os_argv[0], title, g_os_argv_last - g_os_argv[0] - 1);
    g_os_argv[1] = NULL;
}
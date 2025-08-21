/**
 * Process Locking and Instance Check Module
 * Prevents multiple server instances from running simultaneously
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <signal.h>

#include "../include/process_lock.h"
#include "../include/logger.h"

// Global PID file descriptor
static int g_pid_fd = -1;
static char g_pid_file_path[256] = {0};

/**
 * Check if port is available
 */
int check_port_available(int port) {
    int sock_fd;
    struct sockaddr_in addr;
    int result = 0;
    
    // Create test socket
    sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        log_error("Failed to create test socket: %s", strerror(errno));
        return -1;
    }
    
    // Set address
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    
    // Try to bind port
    if (bind(sock_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        if (errno == EADDRINUSE) {
            log_warn("Port %d is already in use", port);
            result = -1;
        } else {
            log_error("Failed to bind test port %d: %s", port, strerror(errno));
            result = -1;
        }
    } else {
        log_info("Port %d is available", port);
        result = 0;
    }
    
    close(sock_fd);
    return result;
}

/**
 * Check if x-server process is running on specified port
 */
int check_xserver_on_port(int port) {
    char cmd[256];
    FILE *fp;
    char line[512];
    int found = 0;
    
    // Use netstat to check port usage
    snprintf(cmd, sizeof(cmd), "netstat -tlnp 2>/dev/null | grep ':%d '", port);
    
    fp = popen(cmd, "r");
    if (fp == NULL) {
        log_warn("Unable to execute netstat command to check port usage");
        return 0;
    }
    
    while (fgets(line, sizeof(line), fp) != NULL) {
        // Check if x-server process name is included
        if (strstr(line, "x-server") != NULL) {
            log_warn("Found x-server process using port %d: %s", port, line);
            found = 1;
            break;
        }
    }
    
    pclose(fp);
    return found;
}

/**
 * Create and lock PID file
 */
int create_pid_file(const char *pid_file, int port) {
    struct flock fl;
    char pid_str[32];
    int len;
    
    // Build PID file path
    if (pid_file != NULL) {
        strncpy(g_pid_file_path, pid_file, sizeof(g_pid_file_path) - 1);
    } else {
        snprintf(g_pid_file_path, sizeof(g_pid_file_path), 
                 "logs/x-server.%d.pid", port);
    }
    
    // Create logs directory (if it doesn't exist)
    mkdir("logs", 0755);
    
    // Open PID file
    g_pid_fd = open(g_pid_file_path, O_RDWR | O_CREAT, 0644);
    if (g_pid_fd < 0) {
        log_error("Failed to create PID file %s: %s", g_pid_file_path, strerror(errno));
        return -1;
    }
    
    // Set file lock
    fl.l_type = F_WRLCK;    // Write lock
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;           // Lock entire file
    
    // Try to acquire lock (non-blocking)
    if (fcntl(g_pid_fd, F_SETLK, &fl) < 0) {
        if (errno == EACCES || errno == EAGAIN) {
            // File is already locked, indicating another instance is running
            char existing_pid[32] = {0};
            lseek(g_pid_fd, 0, SEEK_SET);
            read(g_pid_fd, existing_pid, sizeof(existing_pid) - 1);
            
            log_error("x-server instance already running, PID: %s", existing_pid);
            fprintf(stderr, "Error: x-server instance already running (PID: %s)\n", existing_pid);
            fprintf(stderr, "If you're sure no other instance is running, please delete the PID file: %s\n", g_pid_file_path);
            
            close(g_pid_fd);
            g_pid_fd = -1;
            return -1;
        } else {
            log_error("Failed to lock PID file: %s", strerror(errno));
            close(g_pid_fd);
            g_pid_fd = -1;
            return -1;
        }
    }
    
    // Truncate file and write current PID
    if (ftruncate(g_pid_fd, 0) < 0) {
        log_error("Failed to truncate PID file: %s", strerror(errno));
        close(g_pid_fd);
        g_pid_fd = -1;
        return -1;
    }
    
    len = snprintf(pid_str, sizeof(pid_str), "%d\n", getpid());
    if (write(g_pid_fd, pid_str, len) != len) {
        log_error("Failed to write PID file: %s", strerror(errno));
        close(g_pid_fd);
        g_pid_fd = -1;
        return -1;
    }
    
    // Flush to disk
    fsync(g_pid_fd);
    
    log_info("PID file created successfully: %s (PID: %d)", g_pid_file_path, getpid());
    return 0;
}

/**
 * Check if server instance is already running
 */
int check_server_running(int port) {
    char pid_file[256];
    int fd;
    struct flock fl;
    char pid_str[32] = {0};
    pid_t existing_pid;
    
    // Build PID file path
    snprintf(pid_file, sizeof(pid_file), "logs/x-server.%d.pid", port);
    
    // Try to open PID file
    fd = open(pid_file, O_RDONLY);
    if (fd < 0) {
        if (errno == ENOENT) {
            // PID file doesn't exist, no instance running
            return 0;
        } else {
            log_warn("Unable to open PID file %s: %s", pid_file, strerror(errno));
            return 0;
        }
    }
    
    // Try to acquire read lock
    fl.l_type = F_RDLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;
    
    if (fcntl(fd, F_SETLK, &fl) < 0) {
        if (errno == EACCES || errno == EAGAIN) {
            // File is locked, instance is running
            read(fd, pid_str, sizeof(pid_str) - 1);
            existing_pid = atoi(pid_str);
            
            // Verify if process actually exists
            if (kill(existing_pid, 0) == 0) {
                log_info("Found x-server instance running, PID: %d", existing_pid);
                close(fd);
                return existing_pid;
            } else {
                // Process doesn't exist, might be a zombie PID file
                log_warn("PID file exists but process doesn't exist, might be a zombie PID file: %s", pid_file);
                close(fd);
                unlink(pid_file);  // Delete zombie PID file
                return 0;
            }
        }
    }
    
    close(fd);
    return 0;
}

/**
 * Release PID file lock
 */
void release_pid_file(void) {
    if (g_pid_fd >= 0) {
        // Delete PID file
        unlink(g_pid_file_path);
        
        // Close file descriptor (automatically releases lock)
        close(g_pid_fd);
        g_pid_fd = -1;
        
        log_info("Released PID file: %s", g_pid_file_path);
    }
}

/**
 * Send signal to running server实例
 */
int send_signal_to_running_server(int port, int signal) {
    pid_t server_pid = check_server_running(port);
    
    if (server_pid <= 0) {
        fprintf(stderr, "Found x-server instance running on port: %d\n", port);
        return -1;
    }
    
    if (kill(server_pid, signal) != 0) {
        if (errno == ESRCH) {
            fprintf(stderr, "x-server process %d doesn't exist, cleaning up PID file\n", server_pid);
            char pid_file[256];
            snprintf(pid_file, sizeof(pid_file), "logs/x-server.%d.pid", port);
            unlink(pid_file);
            return -1;
        } else {
            perror("Failed to send signal");
            return -1;
        }
    }
    
    const char *signal_name = "UNKNOWN";
    switch (signal) {
        case SIGHUP: signal_name = "RELOAD"; break;
        case SIGTERM: signal_name = "STOP"; break;
        case SIGQUIT: signal_name = "QUIT"; break;
    }
    
    printf("Sent %s signal to x-server process %d (port %d)\n", signal_name, server_pid, port);
    return 0;
}

/**
 * Comprehensive server pre-start check
 * Note: This function should only be called once in the Master process, Worker processes should not call it
 */
int pre_start_check(int port) {
    // Check if this is a Worker process, if so skip the check
    const char *worker_id = getenv("WORKER_PROCESS_ID");
    if (worker_id != NULL) {
        // This is a Worker process, skip pre-start check
        return 0;
    }
    
    log_info("Starting server pre-start check, port: %d", port);
    
    // 1. Check if instance is already running
    pid_t existing_pid = check_server_running(port);
    if (existing_pid > 0) {
        fprintf(stderr, "Error: x-server instance already running on port %d (PID: %d)\n", port, existing_pid);
        fprintf(stderr, "Please use the following commands to manage existing instance:\n");
        fprintf(stderr, "  Stop service: x-server -s stop\n");
        fprintf(stderr, "  Reload config: x-server -s reload\n");
        fprintf(stderr, "  Check status: ps aux | grep x-server\n");
        return -1;
    }
    
    // 2. Check if port is available
    if (check_port_available(port) != 0) {
        fprintf(stderr, "Error: Port %d is not available\n", port);
        fprintf(stderr, "Please check port usage:\n");
        fprintf(stderr, "  lsof -i :%d\n", port);
        fprintf(stderr, "  netstat -tlnp | grep %d\n", port);
        return -1;
    }
    
    // 3. Check if other x-server processes are using this port
    if (check_xserver_on_port(port) > 0) {
        fprintf(stderr, "Error: Found other x-server process using port %d\n", port);
        return -1;
    }
    
    // 4. Create PID file lock
    if (create_pid_file(NULL, port) != 0) {
        return -1;
    }
    
    log_info("Server pre-start check passed, port: %d", port);
    return 0;
}

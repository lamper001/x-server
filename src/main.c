/**
 * Multi-Process Web Server Main Program - nginx-style architecture
 * 
 * Features:
 * 1. Master process manages configuration and Worker processes
 * 2. Worker processes handle actual HTTP requests
 * 3. Inter-process communication via shared memory
 * 4. Supports hot configuration reload and graceful shutdown
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <getopt.h>
#include <sys/wait.h>
#include <errno.h>

#include "../include/master_process.h"
#include "../include/logger.h"
#include "../include/config.h"
#include "../include/process_title.h"
#include "../include/process_lock.h"

#define DEFAULT_PORT 9001

// Global variables
static char *g_config_file = "config/gateway_multiprocess.conf";
static int g_port = 0;  // 0 means use port from config file
static int g_daemon_mode = 1;

/**
 * Show help information
 */
void show_help(const char *program_name) {
    printf("Usage: %s [options]\n\n", program_name);
    printf("Options:\n");
    printf("  -p <port>       Specify listen port (default: use port from config file, config default: 9001)\n");
    printf("  -c <config>     Specify config file path (default: config/gateway_multiprocess.conf)\n");
    printf("  -f              Run in foreground mode (default: daemon mode)\n");
    printf("  -s <signal>     Send signal to server:\n");
    printf("                    reload: reload configuration\n");
    printf("                    stop: graceful shutdown (wait up to 10 seconds)\n");
    printf("                    quit: force terminate immediately\n");
    printf("  -t              Test configuration file syntax\n");
    printf("  -v              Show version information\n");
    printf("  -h              Show this help information\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s -p 9001 -c config/gateway_multiprocess.conf\n", program_name);
    printf("  %s -s reload\n", program_name);
    printf("  %s -t\n", program_name);
}

/**
 * Show version information
 */
void show_version(void) {
    printf("X-Server Multi-Process Version v2.0\n");
    printf("High-performance web server based on nginx architecture\n");
    printf("Supports multi-process, event-driven, hot configuration reload\n");
}

/**
 * Test configuration file
 */
int test_config(const char *config_file) {
    printf("Testing configuration file: %s\n", config_file);
    
    config_t *config = load_config(config_file);
    if (config == NULL) {
        printf("Configuration file test failed: unable to load config file\n");
        return -1;
    }
    
    printf("Configuration file syntax is correct\n");
    printf("Configuration information:\n");
    printf("  Worker processes: %d\n", config->worker_processes);
    printf("  Route count: %d\n", config->route_count);
    
    for (int i = 0; i < config->route_count; i++) {
        route_t *route = &config->routes[i];
        if (route->type == ROUTE_STATIC) {
            printf("  [%d] %s -> static files (%s)\n", 
                   i + 1, route->path_prefix, route->local_path);
        } else if (route->type == ROUTE_PROXY) {
            printf("  [%d] %s -> proxy (%s:%d)\n", 
                   i + 1, route->path_prefix, route->target_host, route->target_port);
        }
    }
    
    free_config(config);
    return 0;
}

/**
 * Send signal to running server
 */
int send_signal_to_server(const char *signal_name) {
    int sig = 0;
    
    if (strcmp(signal_name, "reload") == 0) {
        sig = SIGHUP;
    } else if (strcmp(signal_name, "stop") == 0) {
        sig = SIGTERM;
    } else if (strcmp(signal_name, "quit") == 0) {
        sig = SIGQUIT;
    } else {
        fprintf(stderr, "Unknown signal: %s\n", signal_name);
        return -1;
    }
    
    // Try multiple common ports to find running instances
    int common_ports[] = {9001, 8080, 3000, 8000, 9000, 0};
    
    for (int i = 0; common_ports[i] != 0; i++) {
        int port = common_ports[i];
        pid_t server_pid = check_server_running(port);
        
        if (server_pid > 0) {
            printf("Found x-server instance running on port %d (PID: %d)\n", port, server_pid);
            return send_signal_to_running_server(port, sig);
        }
    }
    
    // If not found, try to get port from config file
    config_t *config = load_config(g_config_file);
    if (config != NULL) {
        int config_port = config->listen_port;
        free_config(config);
        
        pid_t server_pid = check_server_running(config_port);
        if (server_pid > 0) {
            printf("Found x-server instance running on config port %d (PID: %d)\n", config_port, server_pid);
            return send_signal_to_running_server(config_port, sig);
        }
    }
    
    // Finally try to find by process name
    FILE *fp = popen("pgrep -f 'x-server.*master'", "r");
    if (fp != NULL) {
        char pid_str[16];
        if (fgets(pid_str, sizeof(pid_str), fp) != NULL) {
            pid_t master_pid = atoi(pid_str);
            pclose(fp);
            
            if (kill(master_pid, sig) == 0) {
                const char *signal_name_str = "UNKNOWN";
                switch (sig) {
                    case SIGHUP: signal_name_str = "RELOAD"; break;
                    case SIGTERM: signal_name_str = "STOP"; break;
                    case SIGQUIT: signal_name_str = "QUIT"; break;
                }
                printf("Sent %s signal to x-server Master process %d, graceful shutdown in progress, please wait...\n", signal_name_str, master_pid);
                return 0;
            }
        }
        pclose(fp);
    }
    
    fprintf(stderr, "No running x-server instance found\n");
    fprintf(stderr, "Please check if server is running:\n");
    fprintf(stderr, "  ps aux | grep x-server\n");
    //fprintf(stderr, "  make status\n");
    return -1;
}

/**
 * Set daemon mode
 */
int daemonize(void) {
    pid_t pid = fork();
    
    if (pid < 0) {
        perror("fork failed");
        return -1;
    }
    
    if (pid > 0) {
        // Parent process exits
        exit(0);
    }
    
    // Child process continues
    if (setsid() < 0) {
        perror("setsid failed");
        return -1;
    }
    
    // Fork again to ensure not session leader
    pid = fork();
    if (pid < 0) {
        perror("Second fork failed");
        return -1;
    }
    
    if (pid > 0) {
        exit(0);
    }
    
    // Keep current working directory, do not change to root directory
    // This ensures that relative path config files can be loaded correctly
    
    // Close standard input and output
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
    
    return 0;
}

/**
 * Main function
 */
int main(int argc, char *argv[], char *envp[]) {
    int opt;
    char *signal_name = NULL;
    int test_config_only = 0;
    
    // Initialize process title setting
    init_process_title(argc, argv, envp);
    
    // Parse command line arguments
    while ((opt = getopt(argc, argv, "p:c:fs:tvh")) != -1) {
        switch (opt) {
            case 'p':
                g_port = atoi(optarg);
                if (g_port <= 0 || g_port > 65535) {
                    fprintf(stderr, "Invalid port number: %s\n", optarg);
                    return 1;
                }
                break;
                
            case 'c':
                g_config_file = optarg;
                break;
                
            case 'f':
                g_daemon_mode = 0;
                break;
                
            case 's':
                signal_name = optarg;
                break;
                
            case 't':
                test_config_only = 1;
                break;
                
            case 'v':
                show_version();
                return 0;
                
            case 'h':
                show_help(argv[0]);
                return 0;
                
            default:
                fprintf(stderr, "Unknown option: %c\n", opt);
                show_help(argv[0]);
                return 1;
        }
    }
    
    // Handle signal commands
    if (signal_name != NULL) {
        return send_signal_to_server(signal_name);
    }
    
    // Test configuration file
    if (test_config_only) {
        return test_config(g_config_file);
    }
    
    // Load config file to get log config and port config
    config_t *temp_config = load_config(g_config_file);
    if (temp_config == NULL) {
        fprintf(stderr, "Unable to load config file: %s\n", g_config_file);
        return 1;
    }
    
    // Determine final port to use (command line argument has highest priority)
    int final_port;
    if (g_port > 0) {
        // Use command line specified port
        final_port = g_port;
    } else {
        // Use port from config file
        final_port = temp_config->listen_port;
    }
    
    // Re-check if it's a Worker process (to prevent Worker process from executing here)
    if (getenv("WORKER_PROCESS_ID") != NULL) {
        // Worker process should not execute here, exit immediately
        fprintf(stderr, "Error: Worker process should not execute initialization code in main function\n");
        free_config(temp_config);
        return 1;
    }
    
    // Initialize log system using config file settings
    if (init_logger(temp_config->log_config.log_path, 
                   temp_config->log_config.log_daily, 
                   temp_config->log_config.log_level) != 0) {
        fprintf(stderr, "Failed to initialize log system\n");
        free_config(temp_config);
        return 1;
    }
    
    // Free temporary config
    free_config(temp_config);
    
    printf("X-Server starting...\n");
    printf("Config file: %s\n", g_config_file);
    printf("Listening port: %d\n", final_port);
    
    // Initialize Master process (before switching to daemon mode, so error messages can be displayed)
    if (master_process_init(g_config_file, final_port) != 0) {
        log_error("Master process initialization failed");
        fprintf(stderr, "\n❌ Server startup failed!\n");
        fprintf(stderr, "Please check log files for detailed error information: logs/server.*.log\n");
        fprintf(stderr, "Common issues:\n");
        fprintf(stderr, "  1. Port in use - check if other x-server instances are running\n");
        fprintf(stderr, "  2. Insufficient permissions - ensure sufficient permissions to bind port\n");
        fprintf(stderr, "  3. Configuration error - check if configuration file syntax is correct\n");
        close_logger();
        return 1;
    }
    
    // If not in foreground mode, switch to daemon mode
    // If in daemon mode, switch to daemon mode
    if (g_daemon_mode) {
        printf("Switching to daemon mode...\n");
        if (daemonize() != 0) {
            fprintf(stderr, "Failed to switch to daemon mode\n");
            close_logger();
            return 1;
        }
    }
    
    log_info("X-Server Multi-Process Version started successfully");
    log_info("Master process PID: %d", getpid());
    
    if (!g_daemon_mode) {
        printf("Server started successfully, Master process PID: %d\n", getpid());
        printf("Use Ctrl+C or send SIGTERM signal to gracefully shut down the server\n");
        printf("Use kill -HUP %d to reload configuration\n", getpid());
    }
    
    // Run Master process main loop
    int ret = master_process_run();
    
    log_info("X-Server Multi-Process Version closed, return code: %d", ret);
    close_logger();
    
    if (!g_daemon_mode) {
        printf("Server closed\n");
    }
    
    return ret;
}
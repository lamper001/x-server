/**
 * Master Process Management Module Implementation
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>

#include "../include/master_process.h"
#include "../include/worker_process.h"
#include "../include/shared_memory.h"
#include "../include/logger.h"
#include "../include/process_title.h"
#include "../include/config.h"
#include "../include/process_lock.h"

// Global Master context
static master_context_t *g_master_ctx = NULL;

// Signal handling flags
static volatile sig_atomic_t g_reload_config = 0;
static volatile sig_atomic_t g_shutdown_server = 0;
static volatile sig_atomic_t g_terminate_server = 0;
static volatile sig_atomic_t g_worker_exited = 0;

// Forward declarations
static void master_signal_handler(int sig);
static int setup_master_signals(void);
static int create_listen_socket(int port);
static void cleanup_dead_workers(void);
#ifdef DEBUG
static int respawn_worker_if_needed(worker_process_t *worker);
#endif

/**
 * Master process signal handler
 */
static void master_signal_handler(int sig) {
    switch (sig) {
        case SIGHUP:
            g_reload_config = 1;
            log_info("Master process received SIGHUP signal, preparing to reload configuration");
            break;
            
        case SIGTERM:
        case SIGINT:
            g_shutdown_server = 1;
            log_info("Master process received SIGTERM/SIGINT signal, preparing for graceful shutdown");
            break;
            
        case SIGQUIT:
            g_terminate_server = 1;
            log_info("Master process received SIGQUIT signal, preparing to force terminate");
            break;
            
        case SIGCHLD:
            g_worker_exited = 1;
            break;
            
        default:
            log_warn("Master process received unhandled signal: %d", sig);
            break;
    }
}

/**
 * Set up Master process signal handling
 */
static int setup_master_signals(void) {
    struct sigaction sa;
    
    // Set up signal handler
    sa.sa_handler = master_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    
    if (sigaction(SIGHUP, &sa, NULL) == -1 ||
        sigaction(SIGTERM, &sa, NULL) == -1 ||
        sigaction(SIGINT, &sa, NULL) == -1 ||
        sigaction(SIGQUIT, &sa, NULL) == -1 ||
        sigaction(SIGCHLD, &sa, NULL) == -1) {
        log_error("Failed to set up Master process signal handler: %s", strerror(errno));
        return -1;
    }
    
    // Ignore SIGPIPE signal
    signal(SIGPIPE, SIG_IGN);
    
    return 0;
}

/**
 * Create listening socket
 */
static int create_listen_socket(int port) {
    int listen_fd;
    struct sockaddr_in server_addr;
    int opt = 1;
    
    // Create socket
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        log_error("Failed to create listening socket: %s", strerror(errno));
        return -1;
    }
    
    // Set socket options - only use SO_REUSEADDR, not SO_REUSEPORT
    // SO_REUSEPORT would allow multiple processes to bind to the same port, which is not what we want
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        log_error("Failed to set SO_REUSEADDR: %s", strerror(errno));
        close(listen_fd);
        return -1;
    }
    
    // Bind address
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);
    
    if (bind(listen_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        if (errno == EADDRINUSE) {
            log_error("Failed to bind listening address: port %d is already in use", port);
            fprintf(stderr, "Error: Port %d is already in use by another process, please check if other x-server instances are running\n", port);
            fprintf(stderr, "Hint: You can use the following commands to check port usage:\n");
            fprintf(stderr, "  lsof -i :%d\n", port);
            fprintf(stderr, "  ps aux | grep x-server\n");
        } else {
            log_error("Failed to bind listening address: %s", strerror(errno));
            fprintf(stderr, "Error: Unable to bind port %d: %s\n", port, strerror(errno));
        }
        close(listen_fd);
        return -1;
    }
    
    // Start listening - increase backlog queue size to support high concurrency
    if (listen(listen_fd, 10000) < 0) {
        log_error("Failed to start listening: %s", strerror(errno));
        close(listen_fd);
        return -1;
    }
    
    log_info("Master process successfully created listening socket, port: %d", port);
    return listen_fd;
}

/**
 * Initialize Master process
 */
int master_process_init(const char *config_file, int listen_port) {
    // Perform pre-start checks (port conflict, instance check, PID file locking)
    if (pre_start_check(listen_port) != 0) {
        log_error("Pre-start check failed");
        return -1;
    }
    
    // Allocate Master context
    g_master_ctx = (master_context_t *)malloc(sizeof(master_context_t));
    if (g_master_ctx == NULL) {
        log_error("Failed to allocate Master context memory");
        release_pid_file();  // Release PID file lock
        return -1;
    }
    
    memset(g_master_ctx, 0, sizeof(master_context_t));
    
    // Set basic information
    g_master_ctx->state = MASTER_STARTING;
    g_master_ctx->master_pid = getpid();
    g_master_ctx->start_time = time(NULL);
    
    // Save config file path
    g_master_ctx->config_file = strdup(config_file);
    if (g_master_ctx->config_file == NULL) {
        log_error("Failed to save config file path");
        free(g_master_ctx);
        release_pid_file();
        return -1;
    }
    
    // Load configuration
    g_master_ctx->config = load_config(config_file);
    if (g_master_ctx->config == NULL) {
        log_error("Failed to load config file: %s", config_file);
        free(g_master_ctx->config_file);
        free(g_master_ctx);
        release_pid_file();
        return -1;
    }
    
    // Create listening socket
    g_master_ctx->listen_fd = create_listen_socket(listen_port);
    if (g_master_ctx->listen_fd < 0) {
        free_config(g_master_ctx->config);
        free(g_master_ctx->config_file);
        free(g_master_ctx);
        return -1;
    }
    
    // Initialize shared memory
    if (init_shared_memory() != 0) {
        log_error("Failed to initialize shared memory");
        close(g_master_ctx->listen_fd);
        free_config(g_master_ctx->config);
        free(g_master_ctx->config_file);
        free(g_master_ctx);
        return -1;
    }
    
    // Set up signal handling
    if (setup_master_signals() != 0) {
        cleanup_shared_memory();
        close(g_master_ctx->listen_fd);
        free_config(g_master_ctx->config);
        free(g_master_ctx->config_file);
        free(g_master_ctx);
        return -1;
    }
    
    // Determine Worker processes count
    g_master_ctx->worker_count = g_master_ctx->config->worker_processes;
    if (g_master_ctx->worker_count <= 0) {
        g_master_ctx->worker_count = sysconf(_SC_NPROCESSORS_ONLN);
    }
    
    log_info("Master process initialization completed, PID: %d, Worker processes: %d", 
             g_master_ctx->master_pid, g_master_ctx->worker_count);
    
    return 0;
}

/**
 * Create Worker process
 */
pid_t spawn_worker_process(int worker_id) {
    pid_t pid = fork();
    
    if (pid < 0) {
        log_error("Failed to create Worker process: %s", strerror(errno));
        return -1;
    }
    
    if (pid == 0) {
        // Child process: Worker process
        // Immediately set Worker process environment variable to identify this as a Worker process
        char worker_env[32];
        snprintf(worker_env, sizeof(worker_env), "%d", worker_id);
        setenv("WORKER_PROCESS_ID", worker_env, 1);
        
        // Critical fix: Immediately close file descriptors inherited from parent process to avoid resource leaks
        // But keep the listening socket, Worker process needs to use it
        
        // Set Worker process title
        char title[256];
        snprintf(title, sizeof(title), "x-server: worker process %d", worker_id);
        setproctitle(title);
        
        // Worker process doesn't need to reinitialize log system, directly use inherited configuration
        // Just need to simply record startup information
        log_info("Worker process %d starting, PID: %d", worker_id, getpid());
        
        // Run Worker process main loop
        int ret = worker_process_run(worker_id, g_master_ctx->listen_fd, g_master_ctx->config);
        
        log_info("Worker process %d exited, return code: %d", worker_id, ret);
        
        // Critical fix: Worker process must exit immediately here, cannot continue executing Master process code
        // This is the key to prevent Worker process from repeatedly executing Master process initialization logic
        _exit(ret);  // Use _exit() instead of exit() to avoid executing atexit() registered cleanup functions
    }
    
    // Parent process: Master process
    worker_process_t *worker = (worker_process_t *)malloc(sizeof(worker_process_t));
    if (worker == NULL) {
        log_error("Failed to allocate Worker process info memory");
        kill(pid, SIGTERM);
        return -1;
    }
    
    worker->pid = pid;
    worker->status = 1;
    worker->start_time = time(NULL);
    worker->last_heartbeat = time(NULL);
    worker->respawn_count = 0;
    worker->next = g_master_ctx->workers;
    g_master_ctx->workers = worker;
    
    g_master_ctx->total_workers_spawned++;
    
    log_info("Master process successfully created Worker process, Worker ID: %d, PID: %d", worker_id, pid);
    return pid;
}

/**
 * Clean up dead Worker processes
 */
static void cleanup_dead_workers(void) {
    worker_process_t *worker = g_master_ctx->workers;
    worker_process_t *prev = NULL;
    
    while (worker != NULL) {
        int status;
        pid_t result = waitpid(worker->pid, &status, WNOHANG);
        
        if (result == worker->pid) {
            // Worker process has exited
            log_info("Worker process %d has exited, status: %d", worker->pid, status);
            
            // Remove from linked list
            if (prev == NULL) {
                g_master_ctx->workers = worker->next;
            } else {
                prev->next = worker->next;
            }
            
            worker_process_t *next = worker->next;
            free(worker);
            worker = next;
        } else {
            prev = worker;
            worker = worker->next;
        }
    }
}

/**
 * Restart Worker process if needed
 */
#ifdef DEBUG
static int respawn_worker_if_needed(worker_process_t *worker) {
    if (worker->respawn_count >= 5) {
        log_error("Worker process %d restart count too high, stopping restarts", worker->pid);
        return -1;
    }
    
    time_t now = time(NULL);
    if (now - worker->start_time < 60) {
        // If Worker process exits within 1 minute of startup, increment restart count
        worker->respawn_count++;
    } else {
        // Reset restart count
        worker->respawn_count = 0;
    }
    
    // Restart Worker process
    static int worker_id_counter = 0;
    pid_t new_pid = spawn_worker_process(worker_id_counter++);
    
    return (new_pid > 0) ? 0 : -1;
}
#endif

/**
 * Monitor Worker process status
 */
void monitor_worker_processes(void) {
    // Clean up dead Worker processes
    cleanup_dead_workers();
    
    // Count current active Worker processes
    int active_workers = 0;
    worker_process_t *worker = g_master_ctx->workers;
    while (worker != NULL) {
        active_workers++;
        worker = worker->next;
    }
    
    // If Worker processes count is insufficient, start new Worker processes
    while (active_workers < g_master_ctx->worker_count && 
           g_master_ctx->state == MASTER_RUNNING) {
        static int worker_id_counter = 0;
        if (spawn_worker_process(worker_id_counter++) > 0) {
            active_workers++;
        } else {
            break;
        }
    }
}

/**
 * Reload configuration
 */
int reload_configuration(void) {
    log_info("Starting to reload configuration file: %s", g_master_ctx->config_file);
    
    g_master_ctx->state = MASTER_RELOADING;
    
    // Load new configuration
    config_t *new_config = load_config(g_master_ctx->config_file);
    if (new_config == NULL) {
        log_error("Failed to reload configuration file");
        g_master_ctx->state = MASTER_RUNNING;
        return -1;
    }
    
    // Update configuration in shared memory
    if (update_shared_config(new_config) != 0) {
        log_error("Failed to update shared memory configuration");
        free_config(new_config);
        g_master_ctx->state = MASTER_RUNNING;
        return -1;
    }
    
    // Send reload signal to all Worker processes
    worker_process_t *worker = g_master_ctx->workers;
    while (worker != NULL) {
        if (kill(worker->pid, SIGHUP) != 0) {
            log_warn("Failed to send SIGHUP signal to Worker process %d: %s", 
                     worker->pid, strerror(errno));
        }
        worker = worker->next;
    }
    
    // Free old configuration, use new configuration
    free_config(g_master_ctx->config);
    g_master_ctx->config = new_config;
    g_master_ctx->config_reload_count++;
    
    g_master_ctx->state = MASTER_RUNNING;
    
    log_info("Configuration reload completed, reload count: %d", g_master_ctx->config_reload_count);
    return 0;
}

/**
 * Gracefully shutdown all Worker processes - intelligent waiting strategy
 */
void shutdown_workers_gracefully(void) {
    log_info("Starting graceful shutdown of all Worker processes");
    
    // Send SIGTERM signal to all Worker processes
    worker_process_t *worker = g_master_ctx->workers;
    int worker_count = 0;
    while (worker != NULL) {
        if (kill(worker->pid, SIGTERM) != 0) {
            log_warn("Failed to send SIGTERM signal to Worker process %d: %s", 
                     worker->pid, strerror(errno));
        } else {
            worker_count++;
        }
        worker = worker->next;
    }
    
    log_info("Sent SIGTERM signal to %d Worker processes", worker_count);
    
    // Intelligent waiting strategy:
    // 1. First 2 seconds high-frequency check (idle processes exit quickly)
    // 2. Subsequent normal check (requests in progress complete)
    // 3. Maximum wait 10 seconds (fallback for abnormal situations)
    
    time_t start_time = time(NULL);
    int wait_seconds = 0;
    const int quick_check_time = 2;    // First 2 seconds quick check
    const int max_wait_time = 10;      // Maximum wait 10 seconds
    
    while (g_master_ctx->workers != NULL && wait_seconds < max_wait_time) {
        int initial_worker_count = 0;
        worker = g_master_ctx->workers;
        while (worker != NULL) {
            initial_worker_count++;
            worker = worker->next;
        }
        
        // Clean up exited Worker processes
        cleanup_dead_workers();
        
        int remaining_workers = 0;
        worker = g_master_ctx->workers;
        while (worker != NULL) {
            remaining_workers++;
            worker = worker->next;
        }
        
        // If processes have exited, record immediately
        if (remaining_workers < initial_worker_count) {
            int exited_count = initial_worker_count - remaining_workers;
            log_info("%d Worker processes have exited, %d remaining", exited_count, remaining_workers);
        }
        
        // If all processes have exited, end immediately
        if (remaining_workers == 0) {
            log_info("All Worker processes have exited quickly");
            break;
        }
        
        wait_seconds = time(NULL) - start_time;
        
        // Intelligent wait interval: first 2 seconds high-frequency check, subsequent normal check
        if (wait_seconds < quick_check_time) {
            // First 2 seconds: check every 100ms (idle processes exit quickly)
            usleep(100000);  // 0.1 second
        } else {
            // After 2 seconds: check every 500ms (wait for requests in progress)
            usleep(500000);  // 0.5 second
            
            // Output status every 2 seconds (only when there are remaining processes)
            if (wait_seconds % 2 == 0 && remaining_workers > 0) {
                log_info("Waiting for %d Worker processes to complete current requests... (%d/%d seconds)", 
                         remaining_workers, wait_seconds, max_wait_time);
            }
        }
    }
    
    // If there are still Worker processes that haven't exited, force terminate
    if (g_master_ctx->workers != NULL) {
        int remaining_workers = 0;
        worker = g_master_ctx->workers;
        while (worker != NULL) {
            remaining_workers++;
            worker = worker->next;
        }
        log_warn("Wait timeout, %d Worker processes failed to exit gracefully, force terminating", remaining_workers);
        terminate_workers_forcefully();
    } else {
        log_info("All Worker processes have exited gracefully");
    }
}

/**
 * Force terminate all Worker processes
 */
void terminate_workers_forcefully(void) {
    log_info("Force terminating all Worker processes");
    
    worker_process_t *worker = g_master_ctx->workers;
    int terminated_count = 0;
    
    while (worker != NULL) {
        if (kill(worker->pid, SIGKILL) != 0) {
            if (errno != ESRCH) {  // Process not existing is normal
                log_warn("Failed to force terminate Worker process %d: %s", 
                         worker->pid, strerror(errno));
            }
        } else {
            terminated_count++;
            log_info("Force terminated Worker process %d", worker->pid);
        }
        worker = worker->next;
    }
    
    log_info("Sent SIGKILL signal to %d Worker processes", terminated_count);
    
    // Wait for processes to actually exit, maximum wait 2 seconds
    time_t start_time = time(NULL);
    while (g_master_ctx->workers != NULL && (time(NULL) - start_time) < 2) {
        cleanup_dead_workers();
        usleep(100000);  // 0.1 second
    }
    
    // Finally clean up remaining process records
    cleanup_dead_workers();
    
    if (g_master_ctx->workers != NULL) {
        log_warn("Some Worker process records could not be cleaned up, zombie processes may exist");
    }
}

/**
 * Master process main loop
 */
int master_process_run(void) {
    // Set Master process title
    setproctitle("x-server: master process");
    setproctitle("x-server: master process");
    
    log_info("Master process starting to run, PID: %d", g_master_ctx->master_pid);
    
    g_master_ctx->state = MASTER_RUNNING;
    
    // Start initial Worker processes
    for (int i = 0; i < g_master_ctx->worker_count; i++) {
        if (spawn_worker_process(i) < 0) {
            log_error("Failed to start Worker process");
            return -1;
        }
    }
    
    // Master process main loop
    while (g_master_ctx->state != MASTER_STOPPED) {
        // Check signal flags
        if (g_reload_config) {
            g_reload_config = 0;
            reload_configuration();
        }
        
        if (g_shutdown_server) {
            g_shutdown_server = 0;
            g_master_ctx->state = MASTER_STOPPING;
            log_info("Starting graceful shutdown...");
            shutdown_workers_gracefully();
            break;
        }
        
        if (g_terminate_server) {
            g_terminate_server = 0;
            g_master_ctx->state = MASTER_STOPPING;
            log_info("Starting force terminate server...");
            terminate_workers_forcefully();
            break;
        }
        
        if (g_worker_exited) {
            g_worker_exited = 0;
            monitor_worker_processes();
        }
        
        // Periodically monitor Worker process status
        monitor_worker_processes();
        
        // Check and flush idle log buffers
        logger_check_idle_flush();
        
        // Sleep 1 second
        sleep(1);
    }
    
    g_master_ctx->state = MASTER_STOPPED;
    
    // Clean up resources
    close(g_master_ctx->listen_fd);
    cleanup_shared_memory();
    free_config(g_master_ctx->config);
    free(g_master_ctx->config_file);
    
    // Clean up Worker process linked list
    worker_process_t *worker = g_master_ctx->workers;
    while (worker != NULL) {
        worker_process_t *next = worker->next;
        free(worker);
        worker = next;
    }
    
    free(g_master_ctx);
    g_master_ctx = NULL;
    
    // Release PID file lock
    release_pid_file();
    
    log_info("Master process exited");
    return 0;
}

/**
 * Get Master process statistics
 */
master_context_t *get_master_context(void) {
    return g_master_ctx;
}
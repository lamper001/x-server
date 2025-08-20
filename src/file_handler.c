/**
 * Local File Processing Module Implementation
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>

#include "../include/file_handler.h"
#include "../include/http.h"
#include "../include/config.h"
#include "../include/file_io_enhanced.h"

#define BUFFER_SIZE 8192
#define MAX_PATH_LENGTH 1024

// MIME type mapping table
typedef struct {
    const char *extension;
    const char *mime_type;
} mime_map_t;

// Common file type MIME mapping
static const mime_map_t mime_types[] = {
    {".html", "text/html"},
    {".htm", "text/html"},
    {".css", "text/css"},
    {".js", "application/javascript"},
    {".mjs", "application/javascript"},  // ES modules
    {".json", "application/json"},
    {".map", "application/json"},        // Source Map files
    {".jpg", "image/jpeg"},
    {".jpeg", "image/jpeg"},
    {".png", "image/png"},
    {".gif", "image/gif"},
    {".svg", "image/svg+xml"},
    {".ico", "image/x-icon"},
    {".webp", "image/webp"},             // Modern Web image format
    {".avif", "image/avif"},             // Efficient image format
    {".bmp", "image/bmp"},               // Bitmap format
    {".tiff", "image/tiff"},             // Tagged image format
    {".txt", "text/plain"},
    {".md", "text/markdown"},            // Markdown files
    {".pdf", "application/pdf"},
    {".xml", "application/xml"},
    {".doc", "application/msword"},      // Word documents
    {".docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
    {".xls", "application/vnd.ms-excel"}, // Excel spreadsheets
    {".xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
    {".ppt", "application/vnd.ms-powerpoint"}, // PowerPoint presentations
    {".pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation"},
    {".zip", "application/zip"},
    {".tar", "application/x-tar"},       // Tar archive files
    {".gz", "application/gzip"},         // Gzip compressed files
    {".bz2", "application/x-bzip2"},     // Bzip2 compressed files
    {".7z", "application/x-7z-compressed"}, // 7z compressed files
    {".mp3", "audio/mpeg"},
    {".wav", "audio/wav"},               // WAV audio
    {".ogg", "audio/ogg"},               // OGG audio
    {".flac", "audio/flac"},             // FLAC lossless audio
    {".aac", "audio/aac"},               // AAC audio
    {".mp4", "video/mp4"},
    {".webm", "video/webm"},
    {".avi", "video/x-msvideo"},         // AVI video
    {".mov", "video/quicktime"},         // QuickTime video
    {".flv", "video/x-flv"},             // FLV video
    {".m3u8", "application/x-mpegURL"},  // HLS streaming
    {".ts", "video/MP2T"},               // Transport stream format
    {".woff", "font/woff"},
    {".woff2", "font/woff2"},
    {".ttf", "font/ttf"},
    {".otf", "font/otf"},
    {".eot", "application/vnd.ms-fontobject"},
    {".wasm", "application/wasm"},       // WebAssembly files
    {NULL, NULL}
};

// Get file MIME type
const char *get_mime_type(const char *filename) {
    const char *dot = strrchr(filename, '.');
    if (dot) {
        for (int i = 0; mime_types[i].extension != NULL; i++) {
            if (strcasecmp(dot, mime_types[i].extension) == 0) {
                return mime_types[i].mime_type;
            }
        }
    }
    return "application/octet-stream";  // Default binary type
}

// Send HTTP response headers
static void send_http_header(int client_sock, int status_code, const char *status_text, 
                            const char *content_type, long content_length, const char *charset) {
    char header[BUFFER_SIZE];
    char date_str[100];
    time_t now = time(NULL);
    struct tm *tm_info = gmtime(&now);
    
    strftime(date_str, sizeof(date_str), "%a, %d %b %Y %H:%M:%S GMT", tm_info);
    
    // Build Content-Type, including character set
    char full_content_type[100];
    
    // For text content, add character set
    if (strncmp(content_type, "text/", 5) == 0 || 
        strcmp(content_type, "application/javascript") == 0 ||
        strcmp(content_type, "application/json") == 0 ||
        strcmp(content_type, "application/xml") == 0) {
        snprintf(full_content_type, sizeof(full_content_type), "%s; charset=%s", content_type, charset);
    } else {
        // For binary content, don't add character set
        snprintf(full_content_type, sizeof(full_content_type), "%s", content_type);
    }
    
    // Force all responses to include Connection: close
    int len = snprintf(header, sizeof(header),
                      "HTTP/1.1 %d %s\r\n"
                      "Content-Type: %s\r\n"
                      "Content-Length: %ld\r\n"
                      "Date: %s\r\n"
                      "Server: X-Server\r\n"
                      "Connection: close\r\n"
                      "\r\n",
                      status_code, status_text,
                      full_content_type, content_length,
                      date_str);
    
    // Safely send HTTP headers, ensure complete transmission
    ssize_t total_sent = 0;
    while (total_sent < len) {
        ssize_t bytes_sent = write(client_sock, header + total_sent, len - total_sent);
        if (bytes_sent <= 0) {
            if (errno == EPIPE || errno == ECONNRESET) {
                // Client disconnected
                break;
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Non-blocking write, retry later
                usleep(1000);
                continue;
            } else {
                // Other errors
                break;
            }
        }
        total_sent += bytes_sent;
    }
}

// Send directory listing
static int send_directory_listing(int client_sock, const char *dir_path, const char *url_path, const char *charset, size_t *response_size) {
    DIR *dir;
    struct dirent *entry;
    char buffer[BUFFER_SIZE];
    int len = 0;
    
    dir = opendir(dir_path);
    if (dir == NULL) {
        const char *error_msg = "Unable to open directory";
        send_http_error(client_sock, 500, error_msg, charset);
        if (response_size) *response_size = strlen(error_msg) + 100; // Estimate HTML error page size
        return -1;
    }
    
    // Build HTML page header
    len += snprintf(buffer + len, sizeof(buffer) - len,
                   "<!DOCTYPE html>\r\n"
                   "<html>\r\n"
                   "<head>\r\n"
                   "    <meta charset=\"%s\">\r\n"
                   "    <title>Directory Listing: %s</title>\r\n"
                   "    <style>\r\n"
                   "        body { font-family: Arial, sans-serif; margin: 20px; }\r\n"
                   "        h1 { color: #333; }\r\n"
                   "        ul { list-style-type: none; padding: 0; }\r\n"
                   "        li { margin: 5px 0; }\r\n"
                   "        a { color: #0066cc; text-decoration: none; }\r\n"
                   "        a:hover { text-decoration: underline; }\r\n"
                   "    </style>\r\n"
                   "</head>\r\n"
                   "<body>\r\n"
                   "    <h1>Directory Listing: %s</h1>\r\n"
                   "    <ul>\r\n",
                   charset, url_path, url_path);
    
    // Add parent directory link (if not root directory)
    if (strcmp(url_path, "/") != 0) {
        len += snprintf(buffer + len, sizeof(buffer) - len,
                       "        <li><a href=\"..\">..</a> (Parent Directory)</li>\r\n");
    }
    
    // Add directory and file list
    while ((entry = readdir(dir)) != NULL) {
        // Skip current directory and parent directory
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        // Build complete file path
        char full_path[MAX_PATH_LENGTH];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);
        
        // Get file information
        struct stat file_stat;
        if (stat(full_path, &file_stat) == 0) {
            const char *entry_type = S_ISDIR(file_stat.st_mode) ? " (Directory)" : "";
            
            // Build link URL, ensure it includes current directory path
            char link_url[MAX_PATH_LENGTH];
            if (strcmp(url_path, "/") == 0) {
                // If it's root directory, use filename directly
                snprintf(link_url, sizeof(link_url), "%s%s", 
                         entry->d_name, S_ISDIR(file_stat.st_mode) ? "/" : "");
            } else {
                // If not root directory, need to concatenate current path
                snprintf(link_url, sizeof(link_url), "%s/%s%s", 
                         url_path, entry->d_name, S_ISDIR(file_stat.st_mode) ? "/" : "");
            }
            
            len += snprintf(buffer + len, sizeof(buffer) - len,
                           "        <li><a href=\"%s\">%s</a>%s</li>\r\n",
                           link_url, entry->d_name, entry_type);
        }
    }
    
    // Add HTML page footer
    len += snprintf(buffer + len, sizeof(buffer) - len,
                   "    </ul>\r\n"
                   "</body>\r\n"
                   "</html>\r\n");
    
    closedir(dir);
    
    // Send HTTP response
    send_http_header(client_sock, 200, "OK", "text/html", len, charset);
    
    // Safely send directory listing content
    ssize_t total_sent = 0;
    while (total_sent < len) {
        ssize_t bytes_sent = write(client_sock, buffer + total_sent, len - total_sent);
        if (bytes_sent <= 0) {
            if (errno == EPIPE || errno == ECONNRESET) {
                // Client disconnected
                if (response_size) *response_size = total_sent;
                return -1;
            } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Non-blocking write, retry later
                usleep(1000);
                continue;
            } else {
                // Other errors
                if (response_size) *response_size = total_sent;
                return -1;
            }
        }
        total_sent += bytes_sent;
    }
    
    // Return response size
    if (response_size) *response_size = total_sent;
    
    return 0;
}

// Send file content
static int send_file_content(int client_sock, const char *file_path, const char *charset, size_t *response_size) {
    // Get file information
    struct stat file_stat;
    if (stat(file_path, &file_stat) < 0) {
        const char *error_msg = "File not found";
        send_http_error(client_sock, 404, error_msg, charset);
        if (response_size) *response_size = strlen(error_msg) + 100; // Estimate HTML error page size
        return -1;
    }
    
    // Get file MIME type
    const char *mime_type = get_mime_type(file_path);
    
    // Send HTTP response headers
    send_http_header(client_sock, 200, "OK", mime_type, file_stat.st_size, charset);

    // Use enhanced file I/O module
    size_t total_sent = 0;
    int ret = file_io_enhanced_send_file(client_sock, file_path, &total_sent);
    
    if (ret != 0 || total_sent != (size_t)file_stat.st_size) {
        // If enhanced version fails, fallback to original method
        int file_fd = open(file_path, O_RDONLY);
        if (file_fd < 0) {
            if (response_size) *response_size = total_sent;
            return -1;
        }
        
        ret = sendfile_optimized(client_sock, file_fd, file_stat.st_size, &total_sent);
        close(file_fd);
        
        if (ret != 0 || total_sent != (size_t)file_stat.st_size) {
            if (response_size) *response_size = total_sent;
            return -1;
        }
    }
    
    // Return response size
    if (response_size) *response_size = total_sent;
    return 0;
}

// Check if path is safe (prevent path traversal attacks)
static int is_path_safe(const char *path) {
    // Check if path contains ".." sequence
    const char *ptr = path;
    while (*ptr) {
        if (ptr[0] == '.' && ptr[1] == '.') {
            return 0; // Unsafe
        }
        ptr++;
    }
    
    // Check if path contains other dangerous characters
    if (strchr(path, '\\') || strchr(path, ':')) {
        return 0; // Unsafe
    }
    
    return 1; // Safe
}

// Handle static files requests
int handle_local_file(int client_sock, http_request_t *request, route_t *route, int *status_code, size_t *response_size) {
    if (strlen(route->local_path) == 0) {
        const char *error_msg = "Local file path not configured";
        send_http_error(client_sock, 500, error_msg, route->charset);
        if (status_code) *status_code = 500;
        if (response_size) *response_size = strlen(error_msg) + 100; // Estimate HTML error page size
        return -1;
    }
    
    // Only handle GET and HEAD requests
    if (request->method != HTTP_GET && request->method != HTTP_HEAD) {
        const char *error_msg = "Method not allowed";
        send_http_error(client_sock, 405, error_msg, route->charset);
        if (status_code) *status_code = 405;
        if (response_size) *response_size = strlen(error_msg) + 100; // Estimate HTML error page size
        return -1;
    }
    
    // Build local file path
    char file_path[MAX_PATH_LENGTH];
    
    // Remove path prefix
    const char *relative_path;
    
    // Special handling for root path
    if (strcmp(route->path_prefix, "/") == 0) {
        relative_path = request->path;
        if (*relative_path == '/') {
            relative_path++;  // Skip leading slash
        }
    } else {
        relative_path = request->path + strlen(route->path_prefix);
        if (*relative_path == '/') {
            relative_path++;  // Skip leading slash
        }
    }
    
    // If relative path is empty, use index file or directory listing
    if (*relative_path == '\0') {
        relative_path = ".";
    }
    
    // Check if path is safe
    if (!is_path_safe(relative_path)) {
        const char *error_msg = "Illegal file path";
        send_http_error(client_sock, 403, error_msg, route->charset);
        if (status_code) *status_code = 403;
        if (response_size) *response_size = strlen(error_msg) + 100;
        return -1;
    }
    
    // Build complete file path
    snprintf(file_path, sizeof(file_path), "%s/%s", route->local_path, relative_path);
    
    // Ensure file path doesn't exceed local path scope (prevent path traversal)
    char real_file_path[MAX_PATH_LENGTH];
    char real_local_path[MAX_PATH_LENGTH];
    
    if (realpath(file_path, real_file_path) == NULL || 
        realpath(route->local_path, real_local_path) == NULL) {
        const char *error_msg = "Unable to resolve file path";
        send_http_error(client_sock, 404, error_msg, route->charset);
        if (status_code) *status_code = 404;
        if (response_size) *response_size = strlen(error_msg) + 100;
        return -1;
    }
    
    // Check if file path is within local path scope
    if (strncmp(real_file_path, real_local_path, strlen(real_local_path)) != 0) {
        const char *error_msg = "Access denied";
        send_http_error(client_sock, 403, error_msg, route->charset);
        if (status_code) *status_code = 403;
        if (response_size) *response_size = strlen(error_msg) + 100;
        return -1;
    }
    
    // Check if file exists
    struct stat file_stat;
    if (stat(file_path, &file_stat) < 0) {
        const char *error_msg = "File not found";
        send_http_error(client_sock, 404, error_msg, route->charset);
        if (status_code) *status_code = 404;
        if (response_size) *response_size = strlen(error_msg) + 100; // Estimate HTML error page size
        return -1;
    }
    
    // If it's a directory, send directory listing or find index file
    if (S_ISDIR(file_stat.st_mode)) {
        // Try to find index file
        char index_path[MAX_PATH_LENGTH];
        snprintf(index_path, sizeof(index_path), "%s/index.html", file_path);
        
        if (access(index_path, F_OK) == 0) {
            // Found index file, send it
            if (status_code) *status_code = 200;
            return send_file_content(client_sock, index_path, route->charset, response_size);
        } else {
            // No index file, send directory listing
            if (status_code) *status_code = 200;
            return send_directory_listing(client_sock, file_path, request->path, route->charset, response_size);
        }
    } else {
        // It's a regular file, send file content directly
        if (status_code) *status_code = 200;
        return send_file_content(client_sock, file_path, route->charset, response_size);
    }
}

#if defined(__linux__)
#include <sys/sendfile.h>
#elif defined(__APPLE__)
#include <sys/uio.h>
#endif

int sendfile_optimized(int client_sock, int file_fd, size_t file_size, size_t *sent_bytes) {
    size_t total_sent = 0;
    off_t offset = 0;
    int ret = 0;

#if defined(__linux__)
    // Linux sendfile
    while (total_sent < file_size) {
        n = sendfile(client_sock, file_fd, &offset, file_size - total_sent);
        if (n > 0) {
            total_sent += n;
        } else if (n == 0) {
            break;
        } else {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(1000);
                continue;
            }
            ret = -1;
            break;
        }
    }
#elif defined(__APPLE__)
    // macOS sendfile
    off_t len = file_size;
    int result = sendfile(file_fd, client_sock, offset, &len, NULL, 0);
    if (result == 0) {
        total_sent = len;
    } else {
        if (errno == EAGAIN || errno == EINTR) {
            // May have partially sent, continue trying
            total_sent = len;
        } else {
            ret = -1;
        }
    }
#else
    // Other platforms not supported yet
    ret = -1;
#endif

    // If sendfile is not supported or fails, fallback to read/write
    if (ret == -1 || total_sent < file_size) {
        char buffer[BUFFER_SIZE];
        ssize_t bytes_read, bytes_written;
        size_t written = total_sent;
        lseek(file_fd, total_sent, SEEK_SET); // Position to unsent portion
        while ((bytes_read = read(file_fd, buffer, sizeof(buffer))) > 0) {
            size_t to_write = bytes_read;
            size_t written_now = 0;
            while (written_now < to_write) {
                bytes_written = write(client_sock, buffer + written_now, to_write - written_now);
                if (bytes_written <= 0) {
                    if (errno == EINTR) continue;
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        usleep(1000);
                        continue;
                    }
                    if (sent_bytes) *sent_bytes = written;
                    return -1;
                }
                written_now += bytes_written;
                written += bytes_written;
            }
        }
        total_sent = written;
        ret = 0;
    }
    if (sent_bytes) *sent_bytes = total_sent;
    return ret;
}
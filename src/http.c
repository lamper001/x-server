/**
 * HTTP Request Processing Module - Improved Version
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include "../include/logger.h"
#include "../include/http.h"

#define MAX_REQUEST_SIZE 65536    // Increased to 64KB
#define MAX_HEADERS 100           // Increased header count limit
#define MAX_LINE_LENGTH 8192      // Increased line length limit
#define MAX_URI_LENGTH 2048       // URI length limit

// HTTP method parsing function
http_method_t parse_method(const char *method_str) {
    if (strcmp(method_str, "GET") == 0) return HTTP_GET;
    if (strcmp(method_str, "POST") == 0) return HTTP_POST;
    if (strcmp(method_str, "PUT") == 0) return HTTP_PUT;
    if (strcmp(method_str, "DELETE") == 0) return HTTP_DELETE;
    if (strcmp(method_str, "HEAD") == 0) return HTTP_HEAD;
    if (strcmp(method_str, "OPTIONS") == 0) return HTTP_OPTIONS;
    return HTTP_UNKNOWN;
}

// Get HTTP method string
const char *http_method_str(http_method_t method) {
    switch (method) {
        case HTTP_GET: return "GET";
        case HTTP_POST: return "POST";
        case HTTP_PUT: return "PUT";
        case HTTP_DELETE: return "DELETE";
        case HTTP_HEAD: return "HEAD";
        case HTTP_OPTIONS: return "OPTIONS";
        default: return "UNKNOWN";
    }
}

// Safe read line function (improved version)
static ssize_t safe_read_line(int sock, char *buffer, size_t max_len) {
    size_t i = 0;
    char c;
    ssize_t n;
    
    while (i < max_len - 1) {
        n = recv(sock, &c, 1, 0);
        if (n <= 0) {
            if (n == 0) return i; // Connection closed
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return -1; // Error
        }
        
        if (c == '\n') {
            break;
        }
        
        if (c != '\r') {
            buffer[i++] = c;
        }
    }
    
    buffer[i] = '\0';
    return i;
}

// Read line from buffer
static ssize_t read_line_from_buffer(const char *buffer, size_t buffer_len, size_t *pos, char *line, size_t max_line_len) {
    size_t i = 0;
    size_t start_pos = *pos;
    
    while (*pos < buffer_len && i < max_line_len - 1) {
        char c = buffer[*pos];
        (*pos)++;
        
        if (c == '\n') {
            break;
        }
        
        if (c != '\r') {
            line[i++] = c;
        }
    }
    
    line[i] = '\0';
    return i > 0 || *pos > start_pos ? i : -1;
}

// Safe path normalization - Fix memory leaks and strengthen security checks
static char *normalize_path(const char *path) {
    if (path == NULL) return NULL;
    
    // Pre-check path length to avoid unnecessary memory allocation
    size_t path_len = strlen(path);
    if (path_len == 0) {
        char *result = malloc(2);
        if (result) {
            strcpy(result, "/");
        }
        return result;
    }
    
    // Check path length limit
    if (path_len > MAX_URI_LENGTH) {
        log_warn("Path length exceeds limit: %zu > %d", path_len, MAX_URI_LENGTH);
        return NULL;
    }
    
    // Create path copy for URL decoding check
    char *decoded_path = malloc(path_len + 1);
    if (decoded_path == NULL) {
        log_error("normalize_path: Memory allocation failed");
        return NULL;
    }
    
    // Simple URL decoding, check for dangerous encoded sequences
    const char *src = path;
    char *dst = decoded_path;
    while (*src) {
        if (*src == '%' && src[1] && src[2]) {
            // Check for common encoding attack patterns
            if ((src[1] == '2' && (src[2] == 'e' || src[2] == 'E')) ||  /* %2e = . */
                (src[1] == '2' && (src[2] == 'f' || src[2] == 'F')) ||  /* %2f = / */
                (src[1] == '5' && (src[2] == 'c' || src[2] == 'C'))) {  /* %5c = \ */
                log_warn("Path contains encoded dangerous characters: %s", path);
                free(decoded_path);
                return NULL;
            }
            // Simple decoding (for checking only)
            int hex1 = (src[1] >= '0' && src[1] <= '9') ? src[1] - '0' : 
                      (src[1] >= 'A' && src[1] <= 'F') ? src[1] - 'A' + 10 :
                      (src[1] >= 'a' && src[1] <= 'f') ? src[1] - 'a' + 10 : -1;
            int hex2 = (src[2] >= '0' && src[2] <= '9') ? src[2] - '0' : 
                      (src[2] >= 'A' && src[2] <= 'F') ? src[2] - 'A' + 10 :
                      (src[2] >= 'a' && src[2] <= 'f') ? src[2] - 'a' + 10 : -1;
            if (hex1 >= 0 && hex2 >= 0) {
                *dst++ = (char)(hex1 * 16 + hex2);
                src += 3;
            } else {
                *dst++ = *src++;
            }
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
    
    // Check for dangerous sequences in decoded path
    if (strstr(decoded_path, "../") != NULL || strstr(decoded_path, "..\\") != NULL) {
        log_warn("Path contains dangerous traversal sequences: %s", path);
        free(decoded_path);
        return NULL;
    }
    
    // Check for dangerous characters
    for (size_t i = 0; decoded_path[i]; i++) {
        char c = decoded_path[i];
        if (c == '\n' || c == '\r' || c == '\0' || c < 0x20) {
            log_warn("Path contains dangerous control characters: %s", path);
            free(decoded_path);
            return NULL;
        }
    }
    
    // Check for absolute path attacks (Windows style)
    if (strlen(decoded_path) >= 3 && decoded_path[1] == ':' && 
        ((decoded_path[0] >= 'A' && decoded_path[0] <= 'Z') || 
         (decoded_path[0] >= 'a' && decoded_path[0] <= 'z'))) {
        log_warn("Path contains absolute path: %s", path);
        free(decoded_path);
        return NULL;
    }
    
    char *normalized = malloc(strlen(decoded_path) + 2); // +2 for potential leading '/' and '\0'
    if (normalized == NULL) {
        log_error("normalize_path: Memory allocation failed");
        free(decoded_path);
        return NULL;
    }
    
    src = decoded_path;
    dst = normalized;
    
    // Ensure path starts with /
    if (*src != '/') {
        *dst++ = '/';
    }
    
    while (*src) {
        if (*src == '/') {
            // Skip multiple consecutive slashes
            while (*src == '/') src++;
            if (*src) *dst++ = '/';
        } else if (*src == '.' && (src[1] == '/' || src[1] == '\0')) {
            // Skip ./
            src++;
            if (*src == '/') src++;
        } else {
            *dst++ = *src++;
        }
    }
    
    *dst = '\0';
    
    // Ensure path is not empty
    if (normalized[0] == '\0') {
        normalized[0] = '/';
        normalized[1] = '\0';
    }
    
    free(decoded_path);
    return normalized;
}

// Get request header value
char *get_header_value(http_request_t *request, const char *name) {
    if (request == NULL || name == NULL) return NULL;
    
    for (int i = 0; i < request->header_count; i++) {
        if (strcasecmp(request->headers[i].name, name) == 0) {
            return request->headers[i].value;
        }
    }
    return NULL;
}

// Free HTTP request structure
void free_http_request(http_request_t *request) {
    if (request == NULL) return;
    
    if (request->path) free(request->path);
    if (request->query_string) free(request->query_string);
    if (request->version) free(request->version);
    if (request->body) free(request->body);
    
    if (request->headers != NULL) {
        for (int i = 0; i < request->header_count; i++) {
            if (request->headers[i].name) free(request->headers[i].name);
            if (request->headers[i].value) free(request->headers[i].value);
        }
        free(request->headers);
    }
    
    memset(request, 0, sizeof(http_request_t));
}

// Send nginx-style HTTP error response
void send_http_error(int client_sock, int status_code, const char *message, const char *charset) {
    char html_body[1024];
    char response_headers[512];
    const char *status_text;
    
    // Set status text based on status code
    switch (status_code) {
        case 400: status_text = "Bad Request"; break;
        case 401: status_text = "Unauthorized"; break;
        case 403: status_text = "Forbidden"; break;
        case 404: status_text = "Not Found"; break;
        case 405: status_text = "Method Not Allowed"; break;
        case 500: status_text = "Internal Server Error"; break;
        case 502: status_text = "Bad Gateway"; break;
        default: status_text = "Error"; break;
    }
    
    // Build secure HTML error page - reduce information leakage
    const char *safe_message;
    switch (status_code) {
        case 400:
            safe_message = "Request format error";
            break;
        case 401:
            safe_message = "Authentication required";
            break;
        case 403:
            safe_message = "Access denied";
            break;
        case 404:
            safe_message = "Requested resource not found";
            break;
        case 405:
            safe_message = "Request method not allowed";
            break;
        case 500:
            safe_message = "Internal server error";
            break;
        case 502:
            safe_message = "Gateway error";
            break;
        case 504:
            safe_message = "Gateway timeout";
            break;
        default:
            safe_message = "Server error";
            break;
    }
    
    int html_len = snprintf(html_body, sizeof(html_body),
        "<!DOCTYPE html>\n"
        "<html>\n"
        "<head>\n"
        "    <title>%d %s</title>\n"
        "    <style>\n"
        "        body { font-family: Arial, sans-serif; margin: 40px; background: #f5f5f5; }\n"
        "        .error-container { max-width: 500px; margin: 0 auto; background: white; padding: 40px; border-radius: 8px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }\n"
        "        .error-code { font-size: 48px; font-weight: bold; color: #dc3545; margin-bottom: 20px; text-align: center; }\n"
        "        .error-message { font-size: 18px; margin-bottom: 20px; text-align: center; color: #333; }\n"
        "        .error-details { color: #666; font-size: 14px; text-align: center; }\n"
        "    </style>\n"
        "</head>\n"
        "<body>\n"
        "    <div class=\"error-container\">\n"
        "        <div class=\"error-code\">%d</div>\n"
        "        <div class=\"error-message\">%s</div>\n"
        "        <div class=\"error-details\">%s</div>\n"
        "    </div>\n"
        "</body>\n"
        "</html>",
        status_code, status_text, status_code, status_text, safe_message);
    
    // Check snprintf return value to prevent buffer overflow
    if (html_len < 0 || html_len >= (int)sizeof(html_body)) {
        // If formatting fails, use simple error page
        html_len = snprintf(html_body, sizeof(html_body),
            "<html><body><h1>%d %s</h1><p>%s</p></body></html>",
            status_code, status_text, safe_message);
        if (html_len < 0 || html_len >= (int)sizeof(html_body)) {
            // Final fallback
            strcpy(html_body, "<html><body><h1>Error</h1></body></html>");
            html_len = strlen(html_body);
        }
    }
    
    // Build HTTP response headers - Fix CSS issue, allow inline styles
    int header_len = snprintf(response_headers, sizeof(response_headers),
        "HTTP/1.1 %d %s\r\n"
        "Server: X-Server\r\n"
        "Content-Type: text/html; charset=%s\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "Cache-Control: no-cache, no-store, must-revalidate\r\n"
        "X-Frame-Options: DENY\r\n"
        "X-Content-Type-Options: nosniff\r\n"
        "X-XSS-Protection: 1; mode=block\r\n"
        "Referrer-Policy: strict-origin-when-cross-origin\r\n"
        "Content-Security-Policy: default-src 'self'; style-src 'self' 'unsafe-inline'\r\n"
        "\r\n",
        status_code, status_text, charset ? charset : "UTF-8", html_len);
    
    // Send response headers and body separately to ensure integrity
    ssize_t bytes_written = 0;
    ssize_t total_to_write = header_len;
    
    log_debug("send_http_error: fd=%d, status_code=%d, message=%s", client_sock, status_code, message ? message : "NULL");
    
    // Send response headers
    while (bytes_written < total_to_write) {
        ssize_t n = write(client_sock, response_headers + bytes_written, total_to_write - bytes_written);
        if (n <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(1000);
                continue;
            }
            log_error("Failed to send error page response headers: %s", strerror(errno));
            return;
        }
        bytes_written += n;
    }
    
    // Send response body
    bytes_written = 0;
    total_to_write = html_len;
    while (bytes_written < total_to_write) {
        ssize_t n = write(client_sock, html_body + bytes_written, total_to_write - bytes_written);
        if (n <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(1000);
                continue;
            }
            log_error("Failed to send error page response body: %s", strerror(errno));
            return;
        }
        bytes_written += n;
    }
    
    // Set TCP_NODELAY to ensure immediate sending
    int flag = 1;
    setsockopt(client_sock, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(int));
}

// Parse HTTP request from buffer (for non-blocking I/O)
int parse_http_request_from_buffer(const char *buffer, size_t buffer_len, http_request_t *request) {
    char line[MAX_LINE_LENGTH];
    ssize_t line_len;
    size_t pos = 0;
    size_t total_size = 0;
    
    // Initialize request structure
    memset(request, 0, sizeof(http_request_t));
    request->headers = malloc(sizeof(http_header_t) * MAX_HEADERS);
    if (request->headers == NULL) {
        log_error("Unable to allocate HTTP request header memory");
        return -1;
    }
    request->header_count = 0;
    
    // Read request line
    line_len = read_line_from_buffer(buffer, buffer_len, &pos, line, sizeof(line));
    if (line_len <= 0) {
        free(request->headers);
        request->headers = NULL;
        return -1;
    }
    
    total_size += line_len;
    
    // Safe request line parsing - prevent protocol confusion attacks
    char *method_str = strtok(line, " ");
    char *uri = strtok(NULL, " ");
    char *version = strtok(NULL, " \r\n");
    
    if (method_str == NULL || uri == NULL || version == NULL) {
        log_error("HTTP request line format error: %s", line);
        free_http_request(request);
        return -1;
    }
    
    // Strictly validate HTTP method
    if (strlen(method_str) > 16) { // Limit method length
        log_error("HTTP method too long: %s", method_str);
        free_http_request(request);
        return -1;
    }
    
    // Check for dangerous characters in method
    for (size_t i = 0; method_str[i]; i++) {
        char c = method_str[i];
        if (!isalpha(c)) {
            log_error("HTTP method contains illegal characters: %s", method_str);
            free_http_request(request);
            return -1;
        }
    }
    
    // Strictly validate HTTP version format
    if (strncmp(version, "HTTP/", 5) != 0) {
        log_error("Invalid HTTP version format: %s", version);
        free_http_request(request);
        return -1;
    }
    
    // Check version number format (HTTP/x.y)
    if (strlen(version) != 8 || version[5] < '0' || version[5] > '9' || 
        version[6] != '.' || version[7] < '0' || version[7] > '9') {
        log_error("Invalid HTTP version number: %s", version);
        free_http_request(request);
        return -1;
    }
    
    // Only support HTTP/1.0 and HTTP/1.1
    if (strcmp(version, "HTTP/1.0") != 0 && strcmp(version, "HTTP/1.1") != 0) {
        log_error("Unsupported HTTP version: %s", version);
        free_http_request(request);
        return -1;
    }
    
    // Check URI length
    if (strlen(uri) > MAX_URI_LENGTH) {
        log_error("URI length exceeds limit: %zu > %d", strlen(uri), MAX_URI_LENGTH);
        free_http_request(request);
        return -1;
    }
    
    // Check for dangerous characters in URI
    for (size_t i = 0; uri[i]; i++) {
        char c = uri[i];
        if (c < 0x20 || c == 0x7F) { // Control characters
            log_error("URI contains control characters: %s", uri);
            free_http_request(request);
            return -1;
        }
    }
    
    request->method = parse_method(method_str);
    if (request->method == HTTP_UNKNOWN) {
        log_error("Unsupported HTTP method: %s", method_str);
        free_http_request(request);
        return -1;
    }
    
    // Safe path parsing
    char *query_string = strchr(uri, '?');
    if (query_string != NULL) {
        *query_string = '\0';
        query_string++;
        request->query_string = strdup(query_string);
    } else {
        request->query_string = NULL;
    }
    
    // Normalize path
    request->path = normalize_path(uri);
    if (request->path == NULL) {
        free(request->headers);
        request->headers = NULL;
        if (request->query_string) {
            free(request->query_string);
            request->query_string = NULL;
        }
        return -1;
    }
    
    request->version = strdup(version);
    
    // Read request headers
    while ((line_len = read_line_from_buffer(buffer, buffer_len, &pos, line, sizeof(line))) > 0) {
        total_size += line_len;
        
        // Check total request size
        if (total_size > MAX_REQUEST_SIZE) {
            log_error("HTTP request too large: %zu bytes, exceeds limit %d bytes", total_size, MAX_REQUEST_SIZE);
            free_http_request(request);
            return -1;
        }
        
        // Empty line indicates end of headers
        if (line_len == 0) {
            break;
        }
        
        // Safe header parsing - prevent CRLF injection
        char *colon = strchr(line, ':');
        if (colon == NULL) continue;
        
        *colon = '\0';
        char *name = line;
        char *value = colon + 1;
        
        // Remove leading and trailing spaces
        while (isspace(*name)) name++;
        while (isspace(*value)) value++;
        
        char *end = name + strlen(name) - 1;
        while (end > name && isspace(*end)) *end-- = '\0';
        
        end = value + strlen(value) - 1;
        while (end > value && isspace(*end)) *end-- = '\0';
        
        // Validate header name (prevent injection attacks)
        if (strlen(name) == 0 || strlen(name) > 256) {
            log_warn("Invalid header name length: %zu", strlen(name));
            continue;
        }
        
        // Check for dangerous characters in header name
        for (size_t i = 0; name[i]; i++) {
            char c = name[i];
            if (c < 0x21 || c > 0x7E || c == ':' || c == ' ' || c == '\t') {
                log_warn("Header name contains illegal characters: %s", name);
                goto skip_header;
            }
        }
        
        // Validate header value (prevent CRLF injection)
        if (strlen(value) > 8192) { // Limit header value length
            log_warn("Header value too long: %zu", strlen(value));
            continue;
        }
        
        // Check for CRLF injection
        for (size_t i = 0; value[i]; i++) {
            char c = value[i];
            if (c == '\r' || c == '\n') {
                log_warn("Header value contains CRLF characters, possible injection attack: %s", name);
                goto skip_header;
            }
            // Check other control characters
            if (c < 0x20 && c != '\t') {
                log_warn("Header value contains control characters: %s", name);
                goto skip_header;
            }
        }
        
        // Check for duplicate critical headers (prevent header pollution)
        const char *critical_headers[] = {
            "Content-Length", "Transfer-Encoding", "Host", 
            "Authorization", "Cookie", NULL
        };
        
        for (int i = 0; critical_headers[i]; i++) {
            if (strcasecmp(name, critical_headers[i]) == 0) {
                // Check if this critical header already exists
                for (int j = 0; j < request->header_count; j++) {
                    if (strcasecmp(request->headers[j].name, name) == 0) {
                        log_warn("Duplicate critical header: %s", name);
                        goto skip_header;
                    }
                }
            }
        }
        
        if (strlen(name) > 0 && strlen(value) > 0) {
            if (request->header_count >= MAX_HEADERS) {
                log_warn("Header count exceeds limit: %d", MAX_HEADERS);
                break;
            }
            
            request->headers[request->header_count].name = strdup(name);
            request->headers[request->header_count].value = strdup(value);
            request->header_count++;
        }
        
        skip_header:
        continue;
    }
    
    // Safe request body handling - fix HTTP injection risks
    char *content_length_str = get_header_value(request, "Content-Length");
    char *transfer_encoding_str = get_header_value(request, "Transfer-Encoding");
    
    // Check for Transfer-Encoding and Content-Length conflict (prevent request smuggling)
    if (transfer_encoding_str != NULL && content_length_str != NULL) {
        log_warn("Both Transfer-Encoding and Content-Length headers present, rejecting request");
        free_http_request(request);
        return -1;
    }
    
    // Handle Transfer-Encoding: chunked (prevent request smuggling attacks)
    if (transfer_encoding_str != NULL) {
        if (strcasecmp(transfer_encoding_str, "chunked") == 0) {
            log_warn("Chunked encoding not supported, rejecting request");
            free_http_request(request);
            return -1;
        } else {
            log_warn("Unsupported Transfer-Encoding: %s", transfer_encoding_str);
            free_http_request(request);
            return -1;
        }
    }
    
    if (content_length_str != NULL) {
        // Strictly validate Content-Length format
        char *endptr;
        errno = 0;
        long content_length = strtol(content_length_str, &endptr, 10);
        
        // Check if conversion was successful
        if (errno != 0 || *endptr != '\0' || endptr == content_length_str) {
            log_warn("Invalid Content-Length format: %s", content_length_str);
            free_http_request(request);
            return -1;
        }
        
        // Check for negative values (prevent integer overflow attacks)
        if (content_length < 0) {
            log_warn("Content-Length cannot be negative: %ld", content_length);
            free_http_request(request);
            return -1;
        }
        
        // Check if exceeds maximum limit
        const size_t MAX_BODY_SIZE = 10 * 1024 * 1024; // 10MB limit
        if ((size_t)content_length > MAX_BODY_SIZE) {
            log_warn("Content-Length exceeds maximum limit: %ld > %zu", content_length, MAX_BODY_SIZE);
            free_http_request(request);
            return -1;
        }
        
        // Check if would cause total request size to exceed limit
        if ((size_t)content_length > (MAX_REQUEST_SIZE - total_size)) {
            log_warn("Total request size exceeds limit: %zu + %ld > %d", total_size, content_length, MAX_REQUEST_SIZE);
            free_http_request(request);
            return -1;
        }
        
        if (content_length > 0) {
            // Check if buffer has enough data
            if (buffer_len - pos >= (size_t)content_length) {
                // Safe memory allocation (prevent integer overflow)
                if (content_length < 0 || (size_t)content_length >= SIZE_MAX - 1) {
                    log_error("Content-Length too large, cannot allocate memory");
                    free_http_request(request);
                    return -1;
                }
                
                request->body = malloc((size_t)content_length + 1);
                if (request->body != NULL) {
                    memcpy(request->body, buffer + pos, (size_t)content_length);
                    request->body[content_length] = '\0';
                    request->body_length = (size_t)content_length;
                    pos += (size_t)content_length;
                } else {
                    log_error("Failed to allocate request body memory");
                    free_http_request(request);
                    return -1;
                }
            } else {
                // Not enough data in buffer, need more data
                return -2;  // Special return value indicating need for more data
            }
        }
    }
    
    return 0;
}

// Improved HTTP request parsing - add security protection
int parse_http_request_safe(int client_sock, http_request_t *request) {
    char line[MAX_LINE_LENGTH];
    ssize_t line_len;
    size_t total_size = 0;
    time_t start_time = time(NULL);
    const time_t MAX_REQUEST_TIME = 30; // 30 second timeout
    
    // Initialize request structure
    memset(request, 0, sizeof(http_request_t));
    request->headers = malloc(sizeof(http_header_t) * MAX_HEADERS);
    if (request->headers == NULL) {
        return -1;
    }
    request->header_count = 0;
    
    // Set socket timeout (prevent slow attacks)
    struct timeval timeout;
    timeout.tv_sec = 10;  // 10 second read timeout
    timeout.tv_usec = 0;
    setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    
    // Set socket to non-blocking mode for better timeout control
    int flags = fcntl(client_sock, F_GETFL, 0);
    if (flags != -1) {
        fcntl(client_sock, F_SETFL, flags | O_NONBLOCK);
    }
    
    // Read request line
    line_len = safe_read_line(client_sock, line, sizeof(line));
    if (line_len <= 0) {
        free(request->headers);
        return -1;
    }
    
    total_size += line_len;
    
    // Parse request line
    char *method_str = strtok(line, " ");
    char *uri = strtok(NULL, " ");
    char *version = strtok(NULL, " \r\n");
    
    if (method_str == NULL || uri == NULL || version == NULL) {
        free(request->headers);
        request->headers = NULL;
        return -1;
    }
    
    // Check URI length
    if (strlen(uri) > MAX_URI_LENGTH) {
        free(request->headers);
        request->headers = NULL;
        return -1;
    }
    
    request->method = parse_method(method_str);
    
    // Safe path parsing
    char *query_string = strchr(uri, '?');
    if (query_string != NULL) {
        *query_string = '\0';
        query_string++;
        request->query_string = strdup(query_string);
    } else {
        request->query_string = NULL;
    }
    
    // Normalize path
    request->path = normalize_path(uri);
    if (request->path == NULL) {
        free(request->headers);
        request->headers = NULL;
        if (request->query_string) {
            free(request->query_string);
            request->query_string = NULL;
        }
        return -1;
    }
    
    request->version = strdup(version);
    
    // Read request headers - add timeout check
    while ((line_len = safe_read_line(client_sock, line, sizeof(line))) > 0) {
        // Check for timeout (prevent slow attacks)
        time_t current_time = time(NULL);
        if (current_time - start_time > MAX_REQUEST_TIME) {
            log_warn("HTTP request parsing timeout");
            free_http_request(request);
            return -1;
        }
        
        total_size += line_len;
        
        // Check total request size
        if (total_size > MAX_REQUEST_SIZE) {
            log_warn("HTTP request too large: %zu > %d", total_size, MAX_REQUEST_SIZE);
            free_http_request(request);
            return -1;
        }
        
        // Empty line indicates end of headers
        if (line_len == 0) {
            break;
        }
        
        // Parse request headers
        char *colon = strchr(line, ':');
        if (colon == NULL) continue;
        
        *colon = '\0';
        char *name = line;
        char *value = colon + 1;
        
        // Remove leading and trailing spaces
        while (isspace(*name)) name++;
        while (isspace(*value)) value++;
        
        char *end = name + strlen(name) - 1;
        while (end > name && isspace(*end)) *end-- = '\0';
        
        end = value + strlen(value) - 1;
        while (end > value && isspace(*end)) *end-- = '\0';
        
        if (strlen(name) > 0 && strlen(value) > 0) {
            if (request->header_count >= MAX_HEADERS) {
                break;
            }
            
            request->headers[request->header_count].name = strdup(name);
            request->headers[request->header_count].value = strdup(value);
            request->header_count++;
        }
    }
    
    // Read request body
    char *content_length_str = get_header_value(request, "Content-Length");
    if (content_length_str != NULL) {
        long content_length = atol(content_length_str);
        
        // Limit request body size and fix type comparison warning
        if (content_length > 0 && (size_t)content_length <= (MAX_REQUEST_SIZE - total_size)) {
            request->body = malloc(content_length + 1);
            if (request->body != NULL) {
                size_t total_read = 0;
                
                while (total_read < (size_t)content_length) {
                    ssize_t bytes_read = recv(client_sock, request->body + total_read, 
                                            content_length - total_read, 0);
                    if (bytes_read <= 0) break;
                    total_read += bytes_read;
                }
                
                request->body[total_read] = '\0';
                request->body_length = total_read;
            }
        }
    }
    
    return 0;
}

// Standard HTTP request parsing function (for compatibility)
int parse_http_request(int client_sock, http_request_t *request) {
    return parse_http_request_safe(client_sock, request);
}
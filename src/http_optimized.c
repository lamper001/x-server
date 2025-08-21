/**
 * Optimized HTTP Parser Implementation
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>
#include "../include/http_optimized.h"
#include "../include/logger.h"

// Constant definitions
#define MAX_HEADERS 100
#define MAX_LINE_LENGTH 8192
#define MAX_URI_LENGTH 2048

// Forward declarations
http_method_t parse_method(const char *method_str);
void free_http_request(http_request_t *request);

// Inline function: Get high-precision time (nanoseconds)
static inline uint64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

// Inline function: Safe character check
static inline bool is_http_token_char(char c) {
    return c > 0x1F && c != 0x7F && c != ' ' && c != '\t' && c != '\r' && c != '\n';
}

// Inline function: Safe header value character check
static inline bool is_http_header_value_char(char c) {
    return c >= 0x20 && c != 0x7F;
}

// Create HTTP parser
http_parser_t *http_parser_create(void) {
    http_parser_t *parser = malloc(sizeof(http_parser_t));
    if (!parser) {
        log_error("Failed to allocate HTTP parser memory");
        return NULL;
    }
    
    // Initialize parser
    memset(parser, 0, sizeof(http_parser_t));
    parser->state = HTTP_PARSE_START;
    parser->request = malloc(sizeof(http_request_t));
    
    if (!parser->request) {
        log_error("Failed to allocate HTTP request memory");
        free(parser);
        return NULL;
    }
    
    memset(parser->request, 0, sizeof(http_request_t));
    parser->request->headers = malloc(sizeof(http_header_t) * MAX_HEADERS);
    
    if (!parser->request->headers) {
        log_error("Failed to allocate HTTP headers memory");
        free(parser->request);
        free(parser);
        return NULL;
    }
    
    return parser;
}

// Destroy HTTP parser
void http_parser_destroy(http_parser_t *parser) {
    if (!parser) return;
    
    if (parser->request) {
        free_http_request(parser->request);
        free(parser->request);
    }
    
    free(parser);
}

// Reset HTTP parser state
void http_parser_reset(http_parser_t *parser) {
    if (!parser) return;
    
    parser->state = HTTP_PARSE_START;
    parser->pos = 0;
    parser->line_start = 0;
    parser->header_start = 0;
    parser->has_content_length = false;
    parser->content_length = 0;
    parser->chunked_transfer = false;
    
    // Clear buffers
    memset(parser->method_buffer, 0, sizeof(parser->method_buffer));
    memset(parser->uri_buffer, 0, sizeof(parser->uri_buffer));
    memset(parser->version_buffer, 0, sizeof(parser->version_buffer));
    memset(parser->header_name_buffer, 0, sizeof(parser->header_name_buffer));
    memset(parser->header_value_buffer, 0, sizeof(parser->header_value_buffer));
    
    // Reset request structure
    if (parser->request) {
        free_http_request(parser->request);
        memset(parser->request, 0, sizeof(http_request_t));
        parser->request->headers = malloc(sizeof(http_header_t) * MAX_HEADERS);
        if (!parser->request->headers) {
            log_error("Failed to allocate HTTP headers memory during reset");
        }
    }
}

// Parse HTTP request (state machine version)
ssize_t http_parser_parse(http_parser_t *parser, const char *buffer, size_t buffer_len) {
    if (!parser || !buffer) return -1;
    
    uint64_t start_time = get_time_ns();
    size_t initial_pos = parser->pos;
    
    while (parser->pos < buffer_len && parser->state != HTTP_PARSE_COMPLETE && parser->state != HTTP_PARSE_ERROR) {
        char c = buffer[parser->pos];
        
        switch (parser->state) {
            case HTTP_PARSE_START:
                // Skip leading whitespace characters
                if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                    parser->pos++;
                    continue;
                }
                parser->state = HTTP_PARSE_METHOD;
                parser->line_start = parser->pos;
                // Continue parsing method
                
            case HTTP_PARSE_METHOD:
                if (c == ' ') {
                    // Method parsing completed
                    size_t method_len = parser->pos - parser->line_start;
                    if (method_len >= sizeof(parser->method_buffer)) {
                        parser->state = HTTP_PARSE_ERROR;
                        break;
                    }
                    strncpy(parser->method_buffer, buffer + parser->line_start, method_len);
                    parser->method_buffer[method_len] = '\0';
                    parser->state = HTTP_PARSE_URI;
                    parser->pos++;
                    parser->line_start = parser->pos;
                } else if (!is_http_token_char(c)) {
                    parser->state = HTTP_PARSE_ERROR;
                } else {
                    parser->pos++;
                }
                break;
                
            case HTTP_PARSE_URI:
                if (c == ' ') {
                    // URI parsing completed
                    size_t uri_len = parser->pos - parser->line_start;
                    if (uri_len >= sizeof(parser->uri_buffer)) {
                        parser->state = HTTP_PARSE_ERROR;
                        break;
                    }
                    strncpy(parser->uri_buffer, buffer + parser->line_start, uri_len);
                    parser->uri_buffer[uri_len] = '\0';
                    parser->state = HTTP_PARSE_VERSION;
                    parser->pos++;
                    parser->line_start = parser->pos;
                } else if (c < 0x20 || c == 0x7F) {
                    parser->state = HTTP_PARSE_ERROR;
                } else {
                    parser->pos++;
                }
                break;
                
            case HTTP_PARSE_VERSION:
                if (c == '\r' || c == '\n') {
                    // Version parsing completed
                    size_t version_len = parser->pos - parser->line_start;
                    if (version_len >= sizeof(parser->version_buffer)) {
                        parser->state = HTTP_PARSE_ERROR;
                        break;
                    }
                    strncpy(parser->version_buffer, buffer + parser->line_start, version_len);
                    parser->version_buffer[version_len] = '\0';
                    
                    // Validate version format
                    if (strncmp(parser->version_buffer, "HTTP/", 5) != 0) {
                        parser->state = HTTP_PARSE_ERROR;
                        break;
                    }
                    
                    // Set request method
                    parser->request->method = parse_method(parser->method_buffer);
                    if (parser->request->method == HTTP_UNKNOWN) {
                        parser->state = HTTP_PARSE_ERROR;
                        break;
                    }
                    
                    // Set path
                    parser->request->path = strdup(parser->uri_buffer);
                    if (!parser->request->path) {
                        parser->state = HTTP_PARSE_ERROR;
                        break;
                    }
                    
                    // Parse query string
                    char *query_string = strchr(parser->request->path, '?');
                    if (query_string) {
                        *query_string = '\0';
                        parser->request->query_string = strdup(query_string + 1);
                    }
                    
                    parser->state = HTTP_PARSE_HEADER_NAME;
                    parser->pos++;
                    parser->line_start = parser->pos;
                } else if (!is_http_token_char(c) && c != '.' && c != '/') {
                    parser->state = HTTP_PARSE_ERROR;
                } else {
                    parser->pos++;
                }
                break;
                
            case HTTP_PARSE_HEADER_NAME:
                if (c == '\r' || c == '\n') {
                    // Empty line, header parsing completed
                    if (c == '\r' && parser->pos + 1 < buffer_len && buffer[parser->pos + 1] == '\n') {
                        parser->pos += 2;
                    } else {
                        parser->pos++;
                    }
                    parser->state = HTTP_PARSE_HEADER_END;
                } else if (c == ':') {
                    // Header name parsing completed
                    size_t name_len = parser->pos - parser->line_start;
                    if (name_len >= sizeof(parser->header_name_buffer)) {
                        parser->state = HTTP_PARSE_ERROR;
                        break;
                    }
                    strncpy(parser->header_name_buffer, buffer + parser->line_start, name_len);
                    parser->header_name_buffer[name_len] = '\0';
                    parser->state = HTTP_PARSE_HEADER_VALUE;
                    parser->pos++;
                    parser->line_start = parser->pos;
                } else if (!is_http_token_char(c)) {
                    parser->state = HTTP_PARSE_ERROR;
                } else {
                    parser->pos++;
                }
                break;
                
            case HTTP_PARSE_HEADER_VALUE:
                if (c == '\r' || c == '\n') {
                    // Header value parsing completed
                    size_t value_len = parser->pos - parser->line_start;
                    if (value_len >= sizeof(parser->header_value_buffer)) {
                        parser->state = HTTP_PARSE_ERROR;
                        break;
                    }
                    strncpy(parser->header_value_buffer, buffer + parser->line_start, value_len);
                    parser->header_value_buffer[value_len] = '\0';
                    
                    // Add to request headers
                    if (parser->request->header_count < MAX_HEADERS) {
                        http_header_t *header = &parser->request->headers[parser->request->header_count];
                        header->name = strdup(parser->header_name_buffer);
                        header->value = strdup(parser->header_value_buffer);
                        parser->request->header_count++;
                        
                        // Check special headers
                        if (strcasecmp(parser->header_name_buffer, "Content-Length") == 0) {
                            parser->has_content_length = true;
                            parser->content_length = strtoul(parser->header_value_buffer, NULL, 10);
                        } else if (strcasecmp(parser->header_name_buffer, "Transfer-Encoding") == 0) {
                            if (strcasecmp(parser->header_value_buffer, "chunked") == 0) {
                                parser->chunked_transfer = true;
                            }
                        }
                    }
                    
                    if (c == '\r' && parser->pos + 1 < buffer_len && buffer[parser->pos + 1] == '\n') {
                        parser->pos += 2;
                    } else {
                        parser->pos++;
                    }
                    parser->state = HTTP_PARSE_HEADER_NAME;
                    parser->line_start = parser->pos;
                } else if (!is_http_header_value_char(c)) {
                    parser->state = HTTP_PARSE_ERROR;
                } else {
                    parser->pos++;
                }
                break;
                
            case HTTP_PARSE_HEADER_END:
                // Check if request body needs to be parsed
                if (parser->has_content_length && parser->content_length > 0) {
                    parser->state = HTTP_PARSE_BODY;
                } else if (parser->chunked_transfer) {
                    parser->state = HTTP_PARSE_BODY;
                } else {
                    parser->state = HTTP_PARSE_COMPLETE;
                }
                break;
                
            case HTTP_PARSE_BODY:
                // Simplified handling: if Content-Length exists, check if sufficient
                if (parser->has_content_length) {
                    size_t remaining = buffer_len - parser->pos;
                    if (remaining >= parser->content_length) {
                        // Request body complete
                        parser->request->body = malloc(parser->content_length + 1);
                        if (parser->request->body) {
                            memcpy(parser->request->body, buffer + parser->pos, parser->content_length);
                            parser->request->body[parser->content_length] = '\0';
                            parser->request->body_length = parser->content_length;
                            parser->pos += parser->content_length;
                        }
                        parser->state = HTTP_PARSE_COMPLETE;
                    } else {
                        // Need more data
                        return 0;
                    }
                } else {
                    // No Content-Length, consider parsing complete
                    parser->state = HTTP_PARSE_COMPLETE;
                }
                break;
                
            default:
                parser->state = HTTP_PARSE_ERROR;
                break;
        }
    }
    
    // Update statistics
    parser->bytes_processed += parser->pos - initial_pos;
    if (parser->state == HTTP_PARSE_COMPLETE) {
        parser->parse_count++;
        parser->parse_time_ns += get_time_ns() - start_time;
    }
    
    if (parser->state == HTTP_PARSE_ERROR) {
        return -1;
    } else if (parser->state == HTTP_PARSE_COMPLETE) {
        return parser->pos;
    } else {
        return 0; // Need more data
    }
}

// Batch parse HTTP requests
http_batch_result_t *http_parser_parse_batch(const char *buffer, size_t buffer_len, size_t max_requests) {
    http_batch_result_t *result = malloc(sizeof(http_batch_result_t));
    if (!result) {
        log_error("Failed to allocate batch parse result memory");
        return NULL;
    }
    
    result->requests = malloc(sizeof(http_request_t *) * max_requests);
    if (!result->requests) {
        log_error("Failed to allocate request array memory");
        free(result);
        return NULL;
    }
    
    result->count = 0;
    result->capacity = max_requests;
    result->total_bytes = 0;
    result->parse_time_ns = get_time_ns();
    
    http_parser_t *parser = http_parser_create();
    if (!parser) {
        free(result->requests);
        free(result);
        return NULL;
    }
    
    size_t pos = 0;
    while (pos < buffer_len && result->count < max_requests) {
        http_parser_reset(parser);
        
        ssize_t parsed = http_parser_parse(parser, buffer + pos, buffer_len - pos);
        if (parsed > 0) {
            // Parse successful
            result->requests[result->count] = malloc(sizeof(http_request_t));
            if (result->requests[result->count]) {
                memcpy(result->requests[result->count], parser->request, sizeof(http_request_t));
                // Deep copy headers
                if (parser->request->headers && parser->request->header_count > 0) {
                    result->requests[result->count]->headers = malloc(sizeof(http_header_t) * parser->request->header_count);
                    if (result->requests[result->count]->headers) {
                        for (int i = 0; i < parser->request->header_count; i++) {
                            result->requests[result->count]->headers[i].name = strdup(parser->request->headers[i].name);
                            result->requests[result->count]->headers[i].value = strdup(parser->request->headers[i].value);
                        }
                        result->requests[result->count]->header_count = parser->request->header_count;
                    }
                }
                // Deep copy other fields
                if (parser->request->path) {
                    result->requests[result->count]->path = strdup(parser->request->path);
                }
                if (parser->request->query_string) {
                    result->requests[result->count]->query_string = strdup(parser->request->query_string);
                }
                if (parser->request->body) {
                    result->requests[result->count]->body = malloc(parser->request->body_length + 1);
                    if (result->requests[result->count]->body) {
                        memcpy(result->requests[result->count]->body, parser->request->body, parser->request->body_length);
                        result->requests[result->count]->body[parser->request->body_length] = '\0';
                        result->requests[result->count]->body_length = parser->request->body_length;
                    }
                }
            }
            result->count++;
            pos += parsed;
            result->total_bytes += parsed;
        } else if (parsed == 0) {
            // Need more data
            break;
        } else {
            // Parse error, skip current character
            pos++;
        }
    }
    
    result->parse_time_ns = get_time_ns() - result->parse_time_ns;
    http_parser_destroy(parser);
    
    return result;
}

// Free batch parse result
void http_batch_result_destroy(http_batch_result_t *result) {
    if (!result) return;
    
    for (size_t i = 0; i < result->count; i++) {
        if (result->requests[i]) {
            free_http_request(result->requests[i]);
            free(result->requests[i]);
        }
    }
    
    free(result->requests);
    free(result);
}

// Get parser statistics
void http_parser_get_stats(http_parser_t *parser, uint64_t *parse_time_ns, uint64_t *bytes_processed, uint32_t *parse_count) {
    if (!parser) return;
    
    if (parse_time_ns) *parse_time_ns = parser->parse_time_ns;
    if (bytes_processed) *bytes_processed = parser->bytes_processed;
    if (parse_count) *parse_count = parser->parse_count;
}

// Reset parser statistics
void http_parser_reset_stats(http_parser_t *parser) {
    if (!parser) return;
    
    parser->parse_time_ns = 0;
    parser->bytes_processed = 0;
    parser->parse_count = 0;
}

// Optimized HTTP request parsing (compatible with original interface)
int parse_http_request_optimized(int client_sock, http_request_t *request) {
    char buffer[8192];
    ssize_t bytes_read = recv(client_sock, buffer, sizeof(buffer) - 1, 0);
    
    if (bytes_read <= 0) {
        return -1;
    }
    
    buffer[bytes_read] = '\0';
    return parse_http_request_from_buffer_optimized(buffer, bytes_read, request);
}

// Parse HTTP request from buffer (optimized version)
int parse_http_request_from_buffer_optimized(const char *buffer, size_t buffer_len, http_request_t *request) {
    http_parser_t *parser = http_parser_create();
    if (!parser) {
        return -1;
    }
    
    ssize_t result = http_parser_parse(parser, buffer, buffer_len);
    
    if (result > 0) {
        // Copy parse result
        memcpy(request, parser->request, sizeof(http_request_t));
        
        // Deep copy dynamically allocated content
        if (parser->request->path) {
            request->path = strdup(parser->request->path);
        }
        if (parser->request->query_string) {
            request->query_string = strdup(parser->request->query_string);
        }
        if (parser->request->body) {
            request->body = malloc(parser->request->body_length + 1);
            if (request->body) {
                memcpy(request->body, parser->request->body, parser->request->body_length);
                request->body[parser->request->body_length] = '\0';
            }
        }
        if (parser->request->headers && parser->request->header_count > 0) {
            request->headers = malloc(sizeof(http_header_t) * parser->request->header_count);
            if (request->headers) {
                for (int i = 0; i < parser->request->header_count; i++) {
                    request->headers[i].name = strdup(parser->request->headers[i].name);
                    request->headers[i].value = strdup(parser->request->headers[i].value);
                }
                request->header_count = parser->request->header_count;
            }
        }
        
        http_parser_destroy(parser);
        return 0;
    }
    
    http_parser_destroy(parser);
    return -1;
} 
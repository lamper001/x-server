/**
 * OAuth Authentication Module Implementation
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <ctype.h>
#include <stdarg.h>

#include "../include/oauth.h"
#include "../include/http.h"
#include "../include/config.h"
#include "../include/logger.h"

// Store the last OAuth validation failure error message
static char oauth_error_message[256] = "";
static pthread_mutex_t oauth_error_mutex = PTHREAD_MUTEX_INITIALIZER;

// Set OAuth error message
static void set_oauth_error(const char *format, ...) {
    pthread_mutex_lock(&oauth_error_mutex);
    va_list args;
    va_start(args, format);
    vsnprintf(oauth_error_message, sizeof(oauth_error_message), format, args);
    va_end(args);
    pthread_mutex_unlock(&oauth_error_mutex);
}

// Get the last OAuth validation failure error message
const char *get_oauth_error_message() {
    char *error_message;
    pthread_mutex_lock(&oauth_error_mutex);
    error_message = strdup(oauth_error_message);
    pthread_mutex_unlock(&oauth_error_mutex);
    return error_message;
}

// Free the error message returned by get_oauth_error_message
void free_oauth_error_message(const char *error_message) {
    if (error_message != NULL) {
        free((void*)error_message);
    }
}

// Remove whitespace from both ends of string
static char *trim(char *str) {
    if (str == NULL) return NULL;
    
    // Remove leading whitespace
    while (isspace((unsigned char)*str)) str++;
    
    if (*str == 0) return str;
    
    // Remove trailing whitespace
    char *end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;
    
    // Write new terminator
    *(end + 1) = '\0';
    
    return str;
}

// Parse key-value pair
static int parse_key_value(char *line, char **key, char **value) {
    char *delimiter = strchr(line, '=');
    if (delimiter == NULL) return -1;
    
    *delimiter = '\0';
    *key = trim(line);
    *value = trim(delimiter + 1);
    
    return 0;
}

// Simple MD5 implementation (no OpenSSL dependency)
typedef struct {
    unsigned int state[4];
    unsigned int count[2];
    unsigned char buffer[64];
} MD5_CTX;

#define F(x, y, z) ((x & y) | (~x & z))
#define G(x, y, z) ((x & z) | (y & ~z))
#define H(x, y, z) (x ^ y ^ z)
#define I(x, y, z) (y ^ (x | ~z))

#define ROTATE_LEFT(x, n) ((x << n) | (x >> (32 - n)))

#define FF(a, b, c, d, x, s, ac) { \
    a += F(b, c, d) + x + ac; \
    a = ROTATE_LEFT(a, s); \
    a += b; \
}

#define GG(a, b, c, d, x, s, ac) { \
    a += G(b, c, d) + x + ac; \
    a = ROTATE_LEFT(a, s); \
    a += b; \
}

#define HH(a, b, c, d, x, s, ac) { \
    a += H(b, c, d) + x + ac; \
    a = ROTATE_LEFT(a, s); \
    a += b; \
}

#define II(a, b, c, d, x, s, ac) { \
    a += I(b, c, d) + x + ac; \
    a = ROTATE_LEFT(a, s); \
    a += b; \
}

static void MD5Init(MD5_CTX *context) {
    context->count[0] = context->count[1] = 0;
    context->state[0] = 0x67452301;
    context->state[1] = 0xefcdab89;
    context->state[2] = 0x98badcfe;
    context->state[3] = 0x10325476;
}

static void MD5Transform(unsigned int state[4], unsigned char block[64]);

static void MD5Update(MD5_CTX *context, unsigned char *input, unsigned int inputLen) {
    unsigned int i, index, partLen;

    index = (unsigned int)((context->count[0] >> 3) & 0x3F);
    if ((context->count[0] += ((unsigned int)inputLen << 3)) < ((unsigned int)inputLen << 3))
        context->count[1]++;
    context->count[1] += ((unsigned int)inputLen >> 29);

    partLen = 64 - index;
    if (inputLen >= partLen) {
        memcpy(&context->buffer[index], input, partLen);
        MD5Transform(context->state, context->buffer);
        for (i = partLen; i + 63 < inputLen; i += 64)
            MD5Transform(context->state, &input[i]);
        index = 0;
    } else {
        i = 0;
    }
    memcpy(&context->buffer[index], &input[i], inputLen - i);
}

static void MD5Final(unsigned char digest[16], MD5_CTX *context) {
    unsigned char bits[8];
    unsigned int index, padLen;
    static unsigned char PADDING[64] = {
        0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    };

    // Save number of bits
    memcpy(bits, context->count, 8);

    // Pad out to 56 mod 64
    index = (unsigned int)((context->count[0] >> 3) & 0x3f);
    padLen = (index < 56) ? (56 - index) : (120 - index);
    MD5Update(context, PADDING, padLen);

    // Append length (before padding)
    MD5Update(context, bits, 8);

    // Store state in digest
    memcpy(digest, context->state, 16);
}

static void MD5Transform(unsigned int state[4], unsigned char block[64]) {
    unsigned int a = state[0], b = state[1], c = state[2], d = state[3], x[16];

    // Convert 64-byte block to 16 32-bit values
    for (int i = 0, j = 0; j < 64; i++, j += 4)
        x[i] = ((unsigned int)block[j]) | (((unsigned int)block[j+1]) << 8) |
               (((unsigned int)block[j+2]) << 16) | (((unsigned int)block[j+3]) << 24);

    // Round 1
    FF(a, b, c, d, x[ 0], 7, 0xd76aa478);
    FF(d, a, b, c, x[ 1], 12, 0xe8c7b756);
    FF(c, d, a, b, x[ 2], 17, 0x242070db);
    FF(b, c, d, a, x[ 3], 22, 0xc1bdceee);
    FF(a, b, c, d, x[ 4], 7, 0xf57c0faf);
    FF(d, a, b, c, x[ 5], 12, 0x4787c62a);
    FF(c, d, a, b, x[ 6], 17, 0xa8304613);
    FF(b, c, d, a, x[ 7], 22, 0xfd469501);
    FF(a, b, c, d, x[ 8], 7, 0x698098d8);
    FF(d, a, b, c, x[ 9], 12, 0x8b44f7af);
    FF(c, d, a, b, x[10], 17, 0xffff5bb1);
    FF(b, c, d, a, x[11], 22, 0x895cd7be);
    FF(a, b, c, d, x[12], 7, 0x6b901122);
    FF(d, a, b, c, x[13], 12, 0xfd987193);
    FF(c, d, a, b, x[14], 17, 0xa679438e);
    FF(b, c, d, a, x[15], 22, 0x49b40821);

    // Round 2
    GG(a, b, c, d, x[ 1], 5, 0xf61e2562);
    GG(d, a, b, c, x[ 6], 9, 0xc040b340);
    GG(c, d, a, b, x[11], 14, 0x265e5a51);
    GG(b, c, d, a, x[ 0], 20, 0xe9b6c7aa);
    GG(a, b, c, d, x[ 5], 5, 0xd62f105d);
    GG(d, a, b, c, x[10], 9, 0x02441453);
    GG(c, d, a, b, x[15], 14, 0xd8a1e681);
    GG(b, c, d, a, x[ 4], 20, 0xe7d3fbc8);
    GG(a, b, c, d, x[ 9], 5, 0x21e1cde6);
    GG(d, a, b, c, x[14], 9, 0xc33707d6);
    GG(c, d, a, b, x[ 3], 14, 0xf4d50d87);
    GG(b, c, d, a, x[ 8], 20, 0x455a14ed);
    GG(a, b, c, d, x[13], 5, 0xa9e3e905);
    GG(d, a, b, c, x[ 2], 9, 0xfcefa3f8);
    GG(c, d, a, b, x[ 7], 14, 0x676f02d9);
    GG(b, c, d, a, x[12], 20, 0x8d2a4c8a);

    // Round 3
    HH(a, b, c, d, x[ 5], 4, 0xfffa3942);
    HH(d, a, b, c, x[ 8], 11, 0x8771f681);
    HH(c, d, a, b, x[11], 16, 0x6d9d6122);
    HH(b, c, d, a, x[14], 23, 0xfde5380c);
    HH(a, b, c, d, x[ 1], 4, 0xa4beea44);
    HH(d, a, b, c, x[ 4], 11, 0x4bdecfa9);
    HH(c, d, a, b, x[ 7], 16, 0xf6bb4b60);
    HH(b, c, d, a, x[10], 23, 0xbebfbc70);
    HH(a, b, c, d, x[13], 4, 0x289b7ec6);
    HH(d, a, b, c, x[ 0], 11, 0xeaa127fa);
    HH(c, d, a, b, x[ 3], 16, 0xd4ef3085);
    HH(b, c, d, a, x[ 6], 23, 0x04881d05);
    HH(a, b, c, d, x[ 9], 4, 0xd9d4d039);
    HH(d, a, b, c, x[12], 11, 0xe6db99e5);
    HH(c, d, a, b, x[15], 16, 0x1fa27cf8);
    HH(b, c, d, a, x[ 2], 23, 0xc4ac5665);

    // Round 4
    II(a, b, c, d, x[ 0], 6, 0xf4292244);
    II(d, a, b, c, x[ 7], 10, 0x432aff97);
    II(c, d, a, b, x[14], 15, 0xab9423a7);
    II(b, c, d, a, x[ 5], 21, 0xfc93a039);
    II(a, b, c, d, x[12], 6, 0x655b59c3);
    II(d, a, b, c, x[ 3], 10, 0x8f0ccc92);
    II(c, d, a, b, x[10], 15, 0xffeff47d);
    II(b, c, d, a, x[ 1], 21, 0x85845dd1);
    II(a, b, c, d, x[ 8], 6, 0x6fa87e4f);
    II(d, a, b, c, x[15], 10, 0xfe2ce6e0);
    II(c, d, a, b, x[ 6], 15, 0xa3014314);
    II(b, c, d, a, x[13], 21, 0x4e0811a1);
    II(a, b, c, d, x[ 4], 6, 0xf7537e82);
    II(d, a, b, c, x[11], 10, 0xbd3af235);
    II(c, d, a, b, x[ 2], 15, 0x2ad7d2bb);
    II(b, c, d, a, x[ 9], 21, 0xeb86d391);

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

// Generate MD5 hash
static char *md5_hash(const char *input) {
    MD5_CTX context;
    unsigned char digest[16];
    static char output[33];
    
    MD5Init(&context);
    MD5Update(&context, (unsigned char *)input, strlen(input));
    MD5Final(digest, &context);
    
    for (int i = 0; i < 16; i++) {
        sprintf(&output[i * 2], "%02x", digest[i]);
    }
    
    return output;
}

// Load API authentication config file
api_auth_config_t **load_api_auth_config(const char *filename, int *count) {
    *count = 0; // Initialize count to 0
    
    FILE *file = fopen(filename, "r");
    if (file == NULL) {
        log_error("Failed to open API authentication config file: %s", filename);
        return NULL;
    }
    
    // First count the number of configurations
    char line[1024];
    int config_count = 0;
    
    while (fgets(line, sizeof(line), file)) {
        // Remove newline characters
        size_t len = strlen(line);
        if (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';
        if (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';
        
        // Skip empty lines and comments
        char *trimmed = trim(line);
        if (trimmed[0] == '\0' || trimmed[0] == '#')
            continue;
        
        // New section
        if (trimmed[0] == '[' && trimmed[len-1] == ']') {
            config_count++;
        }
    }
    
    // If no valid configuration found, close file and return NULL
    if (config_count == 0) {
        fclose(file);
        return NULL;
    }
    
    // Reset file pointer
    rewind(file);
    
    // Allocate memory
    api_auth_config_t **configs = malloc(sizeof(api_auth_config_t*) * config_count);
    if (configs == NULL) {
        log_error("Failed to allocate API authentication configuration memory");
        fclose(file);
        return NULL;
    }
    
    for (int i = 0; i < config_count; i++) {
        configs[i] = malloc(sizeof(api_auth_config_t));
        if (configs[i] == NULL) {
            log_error("Failed to allocate API authentication configuration memory");
            // Free already allocated memory
            for (int j = 0; j < i; j++) {
                free(configs[j]);
            }
            free(configs);
            fclose(file);
            return NULL;
        }
        configs[i]->app_key = NULL;
        configs[i]->app_secret = NULL;
        configs[i]->allowed_urls = NULL;
        configs[i]->url_count = 0;
        configs[i]->rate_limit = 0;
    }
    
    // Parse configuration
    int current_config = -1;
    
    while (fgets(line, sizeof(line), file)) {
        // Remove newline characters
        size_t len = strlen(line);
        if (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';
        if (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';
        
        // Skip empty lines and comments
        char *trimmed = trim(line);
        if (trimmed[0] == '\0' || trimmed[0] == '#')
            continue;
        
        // New section
        if (trimmed[0] == '[' && trimmed[len-1] == ']') {
            current_config++;
            
            // Extract app_key
            char app_key[1024];
            strncpy(app_key, trimmed + 1, len - 2);
            app_key[len - 2] = '\0';
            
            configs[current_config]->app_key = strdup(app_key);
            continue;
        }
        
        // If no section yet, skip
        if (current_config < 0) continue;
        
        // Parse key-value pair
        char *key, *value;
        if (parse_key_value(trimmed, &key, &value) != 0)
            continue;
        
        // Parse key-value pair
        if (strcmp(key, "app_secret") == 0) {
            configs[current_config]->app_secret = strdup(value);
        } else if (strcmp(key, "allowed_urls") == 0) {
            // Parse URL list
            char *str = strdup(value);
            char *token;
            char **urls = malloc(sizeof(char*) * 50);  // Maximum 50 URLs
            
            int url_count = 0;
            token = strtok(str, ",");
            while (token != NULL && url_count < 50) {
                urls[url_count] = strdup(trim(token));
                url_count++;
                token = strtok(NULL, ",");
            }
            
            configs[current_config]->allowed_urls = urls;
            configs[current_config]->url_count = url_count;
            free(str);
        } else if (strcmp(key, "rate_limit") == 0) {
            configs[current_config]->rate_limit = atoi(value);
        }
    }
    
    fclose(file);
    *count = config_count;
    return configs;
}

// Find matching API authentication configuration
api_auth_config_t *find_api_auth_config(api_auth_config_t **configs, int count, const char *app_key) {
    for (int i = 0; i < count; i++) {
        if (strcmp(configs[i]->app_key, app_key) == 0) {
            return configs[i];
        }
    }
    return NULL;
}

// Check if URL is in allowed list
int is_url_allowed(api_auth_config_t *config, const char *url) {
    // If all URLs are allowed
    for (int i = 0; i < config->url_count; i++) {
        if (strcmp(config->allowed_urls[i], "*") == 0) {
            return 1;
        }
    }
    
    // Check specific URLs
    for (int i = 0; i < config->url_count; i++) {
        char *pattern = config->allowed_urls[i];
        int pattern_len = strlen(pattern);
        
        // If pattern ends with *, do prefix matching
        if (pattern[pattern_len - 1] == '*') {
            int prefix_len = pattern_len - 1;
            if (strncmp(url, pattern, prefix_len) == 0) {
                return 1;
            }
        } 
        // Otherwise do exact matching
        else if (strcmp(url, pattern) == 0) {
            return 1;
        }
    }
    
    return 0;
}

// Free API authentication configuration
void free_api_auth_config(api_auth_config_t **configs, int count) {
    for (int i = 0; i < count; i++) {
        free(configs[i]->app_key);
        free(configs[i]->app_secret);
        
        for (int j = 0; j < configs[i]->url_count; j++) {
            free(configs[i]->allowed_urls[j]);
        }
        free(configs[i]->allowed_urls);
        free(configs[i]);
    }
    free(configs);
}

    // Global variables, store API authentication configuration
static api_auth_config_t **g_configs = NULL;
static int g_config_count = 0;
static pthread_mutex_t g_config_mutex = PTHREAD_MUTEX_INITIALIZER;
static time_t g_last_load_time = 0;

// Initialize OAuth configuration, called when server starts - fix thread safety issues
int init_oauth_config() {
    pthread_mutex_lock(&g_config_mutex);
    
    // Load new configuration to temporary variables
    int temp_config_count = 0;
    api_auth_config_t **temp_configs = load_api_auth_config("config/api_auth.conf", &temp_config_count);
    
    if (temp_configs != NULL) {
        // Save reference to old configuration
        api_auth_config_t **old_configs = g_configs;
        int old_config_count = g_config_count;
        
        // Atomically update global configuration
        g_configs = temp_configs;
        g_config_count = temp_config_count;
        g_last_load_time = time(NULL);
        
        // Free old configuration (after update, ensuring thread safety)
        if (old_configs != NULL) {
            free_api_auth_config(old_configs, old_config_count);
        }
        
        log_info("Successfully loaded OAuth configuration, total %d applications", g_config_count);
        pthread_mutex_unlock(&g_config_mutex);
        return 0;
    } else {
        log_error("Failed to load OAuth configuration");
        pthread_mutex_unlock(&g_config_mutex);
        return -1;
    }
}

// Reload OAuth configuration, can be triggered by -s reload parameter
int reload_oauth_config() {
    log_info("Reloading OAuth configuration...");
    return init_oauth_config();
}

// Validate OAuth request
int validate_oauth(http_request_t *request, route_t *route) {
    (void)route; // avoid unused parameter warning
    // Clear error message
    set_oauth_error("");
    
    // Get OAuth parameters from request headers
    char *auth_app_key = NULL;
    char *auth_token = NULL;
    char *auth_time = NULL;
    char *auth_random = NULL;
    
    // Get OAuth parameters from request headers
    for (int i = 0; i < request->header_count; i++) {
        if (strcasecmp(request->headers[i].name, "oauth-app-key") == 0) {
            auth_app_key = request->headers[i].value;
        } else if (strcasecmp(request->headers[i].name, "oauth-token") == 0) {
            auth_token = request->headers[i].value;
        } else if (strcasecmp(request->headers[i].name, "oauth-time") == 0) {
            auth_time = request->headers[i].value;
        } else if (strcasecmp(request->headers[i].name, "oauth-random") == 0) {
            auth_random = request->headers[i].value;
        }
    }
    
    // Check if all necessary parameters exist
    if (auth_app_key == NULL || auth_token == NULL || auth_time == NULL || auth_random == NULL) {
        // Record detailed error information, including request path and all headers
        log_warn("OAuth validation failed: missing necessary authentication parameters, request path: %s", request->path);
        log_warn("Request header information (total: %d):", request->header_count);
        for (int i = 0; i < request->header_count; i++) {
            log_warn("  %s: %s", request->headers[i].name, request->headers[i].value);
        }
        
        set_oauth_error("Missing necessary authentication parameters, please ensure oauth-app-key, oauth-token, oauth-time and oauth-random headers are included");
        return 0;
    }
    
    // Use globally cached configuration - fix thread safety issues
    pthread_mutex_lock(&g_config_mutex);
    
    // If configuration not loaded, try to load
    if (g_configs == NULL) {
        int temp_config_count = 0;
        api_auth_config_t **temp_configs = load_api_auth_config("config/api_auth.conf", &temp_config_count);
        
        if (temp_configs != NULL) {
            g_configs = temp_configs;
            g_config_count = temp_config_count;
            g_last_load_time = time(NULL);
        } else {
            pthread_mutex_unlock(&g_config_mutex);
            set_oauth_error("Failed to load API authentication configuration");
            log_error("Failed to load API authentication configuration");
            return 0;
        }
    }
    
    // Create local copy of configuration to avoid holding lock for long time
    api_auth_config_t *local_config = NULL;
    for (int i = 0; i < g_config_count; i++) {
        if (strcmp(g_configs[i]->app_key, auth_app_key) == 0) {
            // Create copy of configuration
            local_config = malloc(sizeof(api_auth_config_t));
            if (local_config != NULL) {
                local_config->app_key = strdup(g_configs[i]->app_key);
                local_config->app_secret = strdup(g_configs[i]->app_secret);
                local_config->rate_limit = g_configs[i]->rate_limit;
                local_config->url_count = g_configs[i]->url_count;
                
                // Copy URL list
                local_config->allowed_urls = malloc(sizeof(char*) * local_config->url_count);
                if (local_config->allowed_urls != NULL) {
                    for (int j = 0; j < local_config->url_count; j++) {
                        local_config->allowed_urls[j] = strdup(g_configs[i]->allowed_urls[j]);
                    }
                }
            }
            break;
        }
    }
    
    pthread_mutex_unlock(&g_config_mutex);
    
    // Use local configuration copy for validation
    api_auth_config_t *config = local_config;
    
    if (config == NULL) {
        set_oauth_error("Application key (app_key) does not exist: %s", auth_app_key);
        log_warn("OAuth validation failed: app_key does not exist: %s", auth_app_key);
        // Clean up local configuration copy
        if (local_config != NULL) {
            if (local_config->app_key) free(local_config->app_key);
            if (local_config->app_secret) free(local_config->app_secret);
            if (local_config->allowed_urls) {
                for (int i = 0; i < local_config->url_count; i++) {
                    if (local_config->allowed_urls[i]) free(local_config->allowed_urls[i]);
                }
                free(local_config->allowed_urls);
            }
            free(local_config);
        }
        return 0;
    }
    
    // Check if timestamp is within valid period (5 minutes)
    time_t current_time = time(NULL);
    time_t auth_time_value = atol(auth_time);
    if (current_time - auth_time_value > 300) {  // 5 minutes = 300 seconds
        set_oauth_error("Authentication timestamp has expired");
        log_warn("OAuth validation failed: timestamp expired");
        // Clean up local configuration copy
        if (local_config != NULL) {
            if (local_config->app_key) free(local_config->app_key);
            if (local_config->app_secret) free(local_config->app_secret);
            if (local_config->allowed_urls) {
                for (int i = 0; i < local_config->url_count; i++) {
                    if (local_config->allowed_urls[i]) free(local_config->allowed_urls[i]);
                }
                free(local_config->allowed_urls);
            }
            free(local_config);
        }
        return 0;
    }
    
    // Generate expected token
    char token_input[1024];
    snprintf(token_input, sizeof(token_input), "%s%s%s%s", 
             auth_app_key, config->app_secret, auth_time, auth_random);
    
    char *expected_token = md5_hash(token_input);
    
    // Use constant time comparison to prevent timing attacks
    int token_match = 1;
    size_t expected_len = strlen(expected_token);
    size_t actual_len = strlen(auth_token);
    
    // First compare lengths
    if (expected_len != actual_len) {
        token_match = 0;
    }
    
    // Constant time string comparison
    for (size_t i = 0; i < expected_len && i < actual_len; i++) {
        if (expected_token[i] != auth_token[i]) {
            token_match = 0;
        }
    }
    
    if (!token_match) {
        set_oauth_error("Authentication token does not match");
        log_warn("OAuth validation failed: token does not match");
        // Clean up local configuration copy
        if (local_config != NULL) {
            if (local_config->app_key) free(local_config->app_key);
            if (local_config->app_secret) free(local_config->app_secret);
            if (local_config->allowed_urls) {
                for (int i = 0; i < local_config->url_count; i++) {
                    if (local_config->allowed_urls[i]) free(local_config->allowed_urls[i]);
                }
                free(local_config->allowed_urls);
            }
            free(local_config);
        }
        return 0;
    }
    
    // Check if URL is in allowed list
    if (!is_url_allowed(config, request->path)) {
        set_oauth_error("Requested URL is not in allowed access list: %s", request->path);
        log_warn("OAuth validation failed: URL not in allowed list: %s", request->path);
        // Clean up local configuration copy
        if (local_config != NULL) {
            if (local_config->app_key) free(local_config->app_key);
            if (local_config->app_secret) free(local_config->app_secret);
            if (local_config->allowed_urls) {
                for (int i = 0; i < local_config->url_count; i++) {
                    if (local_config->allowed_urls[i]) free(local_config->allowed_urls[i]);
                }
                free(local_config->allowed_urls);
            }
            free(local_config);
        }
        return 0;
    }
    
    // TODO: Implement rate limiting check
    
    // Validation passed
    log_info("OAuth validation successful: %s", auth_app_key);
    
    // Clean up local configuration copy
    if (local_config != NULL) {
        if (local_config->app_key) free(local_config->app_key);
        if (local_config->app_secret) free(local_config->app_secret);
        if (local_config->allowed_urls) {
            for (int i = 0; i < local_config->url_count; i++) {
                if (local_config->allowed_urls[i]) free(local_config->allowed_urls[i]);
            }
            free(local_config->allowed_urls);
        }
        free(local_config);
    }
    
    return 1;
}


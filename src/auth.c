/**
 * Authentication Module Implementation
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/auth.h"
#include "../include/http.h"
#include "../include/config.h"
#include "../include/oauth.h"
#include "../include/logger.h"

// Get authentication token from HTTP request
char *get_auth_token(http_request_t *request) {
    // Try to get token from Authorization header
    char *auth_header = get_header_value(request, "Authorization");
    if (auth_header != NULL) {
        // Check if it's a Bearer token
        if (strncasecmp(auth_header, "Bearer ", 7) == 0) {
            return auth_header + 7;  // Skip "Bearer " prefix
        }
        return auth_header;
    }
    
    // Try to get token from query string
    if (request->query_string != NULL) {
        char *query = strdup(request->query_string);
        char *token_param = strstr(query, "token=");
        if (token_param != NULL) {
            token_param += 6;  // Skip "token="
            
            // Find parameter end position
            char *end = strchr(token_param, '&');
            if (end != NULL) {
                *end = '\0';
            }
            
            char *token = strdup(token_param);
            free(query);
            return token;
        }
        free(query);
    }
    
    return NULL;
}

// Validate if request is valid
int validate_request(http_request_t *request, route_t *route, auth_result_t *result) {
    // Initialize authentication result
    result->success = 0;
    strcpy(result->error_message, "");
    
    if (route->auth_type == AUTH_NONE) {
        result->success = 1;
        return 1;  // No authentication required
    }
    
    // Choose different validation methods based on authentication type
    switch (route->auth_type) {
        case AUTH_OAUTH:
            // Use OAuth authentication
            {
                int oauth_result = validate_oauth(request, route);
                if (!oauth_result) {
                    const char *error_msg = get_oauth_error_message();
                    strcpy(result->error_message, error_msg);
                    free_oauth_error_message(error_msg);
                } else {
                    result->success = 1;
                }
                return oauth_result;
            }
            
        case AUTH_NONE:
        default:
            // No authentication required or unknown authentication type
            result->success = 1;
            return 1;
    }
}

// Validate if token is valid (for simple token authentication, deprecated)
int validate_token(route_t *route, const char *token, auth_result_t *result) {
    (void)route;  // avoid unused parameter warning
    (void)token;
    
    if (result != NULL) {
        strcpy(result->error_message, "Token authentication is deprecated, please use OAuth authentication");
    }
    
    log_warn("Token authentication is deprecated, please use OAuth authentication");
    return 0;
}
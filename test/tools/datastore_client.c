// src/tools/datastore_client.c
// Command-line client for Roole distributed datastore
// Implements all datastore RPC operations: SET, GET, UNSET, LIST

#define _POSIX_C_SOURCE 200809L

#include "roole/rpc/rpc_client.h"
#include "roole/rpc/rpc_types.h"
#include "roole/core/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <endian.h>
#include <unistd.h>

// ============================================================================
// COLORS FOR OUTPUT
// ============================================================================

#define COLOR_RED     "\033[0;31m"
#define COLOR_GREEN   "\033[0;32m"
#define COLOR_YELLOW  "\033[1;33m"
#define COLOR_BLUE    "\033[0;34m"
#define COLOR_CYAN    "\033[0;36m"
#define COLOR_RESET   "\033[0m"

#define PRINT_SUCCESS(fmt, ...) printf(COLOR_GREEN "✓ " COLOR_RESET fmt "\n", ##__VA_ARGS__)
#define PRINT_ERROR(fmt, ...)   fprintf(stderr, COLOR_RED "✗ " COLOR_RESET fmt "\n", ##__VA_ARGS__)
#define PRINT_INFO(fmt, ...)    printf(COLOR_BLUE "ℹ " COLOR_RESET fmt "\n", ##__VA_ARGS__)
#define PRINT_DATA(fmt, ...)    printf(COLOR_CYAN "→ " COLOR_RESET fmt "\n", ##__VA_ARGS__)

// ============================================================================
// RPC REQUEST BUILDERS
// ============================================================================

// Build SET request: [key_len:2][key][value_len:4][value]
static size_t build_set_request(const char *key, const char *value, 
                                uint8_t *buffer, size_t buffer_size) {
    if (!key || !value || !buffer) return 0;
    
    uint16_t key_len = strlen(key);
    uint32_t value_len = strlen(value);
    
    size_t required = 2 + key_len + 4 + value_len;
    if (required > buffer_size) {
        PRINT_ERROR("Buffer too small for SET request");
        return 0;
    }
    
    uint8_t *ptr = buffer;
    
    // Key length (network byte order)
    uint16_t key_len_net = htons(key_len);
    memcpy(ptr, &key_len_net, 2);
    ptr += 2;
    
    // Key
    memcpy(ptr, key, key_len);
    ptr += key_len;
    
    // Value length (network byte order)
    uint32_t value_len_net = htonl(value_len);
    memcpy(ptr, &value_len_net, 4);
    ptr += 4;
    
    // Value
    memcpy(ptr, value, value_len);
    ptr += value_len;
    
    return ptr - buffer;
}

// Build GET request: [key_len:2][key]
static size_t build_get_request(const char *key, uint8_t *buffer, size_t buffer_size) {
    if (!key || !buffer) return 0;
    
    uint16_t key_len = strlen(key);
    
    size_t required = 2 + key_len;
    if (required > buffer_size) {
        PRINT_ERROR("Buffer too small for GET request");
        return 0;
    }
    
    uint8_t *ptr = buffer;
    
    // Key length
    uint16_t key_len_net = htons(key_len);
    memcpy(ptr, &key_len_net, 2);
    ptr += 2;
    
    // Key
    memcpy(ptr, key, key_len);
    ptr += key_len;
    
    return ptr - buffer;
}

// Build UNSET request: [key_len:2][key]
static size_t build_unset_request(const char *key, uint8_t *buffer, size_t buffer_size) {
    // Same format as GET
    return build_get_request(key, buffer, buffer_size);
}

// Build LIST request: empty (or optional prefix - not implemented yet)
static size_t build_list_request(uint8_t *buffer, size_t buffer_size) {
    (void)buffer;
    (void)buffer_size;
    return 0;  // Empty request
}

// ============================================================================
// RPC RESPONSE PARSERS
// ============================================================================

// Parse SET response: [ack:1][version:8]
static void parse_set_response(const uint8_t *response, size_t response_len) {
    if (response_len < 9) {
        PRINT_ERROR("Invalid SET response length: %zu (expected 9)", response_len);
        return;
    }
    
    uint8_t ack = response[0];
    
    uint64_t version_net;
    memcpy(&version_net, response + 1, 8);
    uint64_t version = be64toh(version_net);
    
    if (ack == 1) {
        PRINT_SUCCESS("Record set successfully");
        PRINT_INFO("Version: %lu", version);
    } else {
        PRINT_ERROR("SET operation failed (ack = %u)", ack);
    }
}

// Parse GET response: [found:1][value_len:4][value][version:8]
static void parse_get_response(const uint8_t *response, size_t response_len) {
    if (response_len < 1) {
        PRINT_ERROR("Invalid GET response length: %zu", response_len);
        return;
    }
    
    uint8_t found = response[0];
    
    if (found == 0) {
        PRINT_INFO("Record not found");
        return;
    }
    
    if (response_len < 1 + 4) {
        PRINT_ERROR("Invalid GET response (truncated)");
        return;
    }
    
    // Parse value length
    uint32_t value_len_net;
    memcpy(&value_len_net, response + 1, 4);
    uint32_t value_len = ntohl(value_len_net);
    
    if (response_len < 1 + 4 + value_len + 8) {
        PRINT_ERROR("Invalid GET response (incomplete value)");
        return;
    }
    
    // Extract value
    char *value = malloc(value_len + 1);
    if (!value) {
        PRINT_ERROR("Failed to allocate memory for value");
        return;
    }
    
    memcpy(value, response + 1 + 4, value_len);
    value[value_len] = '\0';
    
    // Parse version
    uint64_t version_net;
    memcpy(&version_net, response + 1 + 4 + value_len, 8);
    uint64_t version = be64toh(version_net);
    
    PRINT_SUCCESS("Record found");
    PRINT_DATA("Value: %s", value);
    PRINT_INFO("Version: %lu, Size: %u bytes", version, value_len);
    
    free(value);
}

// Parse UNSET response: [ack:1]
static void parse_unset_response(const uint8_t *response, size_t response_len) {
    if (response_len < 1) {
        PRINT_ERROR("Invalid UNSET response length: %zu", response_len);
        return;
    }
    
    uint8_t ack = response[0];
    
    if (ack == 1) {
        PRINT_SUCCESS("Record deleted successfully");
    } else {
        PRINT_ERROR("UNSET operation failed (ack = %u)", ack);
    }
}

// Parse LIST response: [count:4][key1_len:2][key1]...[keyN_len:2][keyN]
static void parse_list_response(const uint8_t *response, size_t response_len) {
    if (response_len < 4) {
        PRINT_ERROR("Invalid LIST response length: %zu", response_len);
        return;
    }
    
    // Parse count
    uint32_t count_net;
    memcpy(&count_net, response, 4);
    uint32_t count = ntohl(count_net);
    
    PRINT_SUCCESS("Found %u keys", count);
    
    if (count == 0) {
        PRINT_INFO("Datastore is empty");
        return;
    }
    
    const uint8_t *ptr = response + 4;
    const uint8_t *end = response + response_len;
    
    printf("\n");
    printf("Keys:\n");
    printf("═══════════════════════════════════════\n");
    
    for (uint32_t i = 0; i < count; i++) {
        if (ptr + 2 > end) {
            PRINT_ERROR("Truncated LIST response at key %u", i);
            break;
        }
        
        // Parse key length
        uint16_t key_len_net;
        memcpy(&key_len_net, ptr, 2);
        uint16_t key_len = ntohs(key_len_net);
        ptr += 2;
        
        if (ptr + key_len > end) {
            PRINT_ERROR("Truncated key data at key %u", i);
            break;
        }
        
        // Extract key
        char *key = malloc(key_len + 1);
        if (!key) {
            PRINT_ERROR("Failed to allocate memory for key");
            break;
        }
        
        memcpy(key, ptr, key_len);
        key[key_len] = '\0';
        ptr += key_len;
        
        printf("  %3u. %s\n", i + 1, key);
        
        free(key);
    }
    
    printf("═══════════════════════════════════════\n");
}

// ============================================================================
// OPERATION HANDLERS
// ============================================================================

static int handle_set(rpc_client_t *client, const char *key, const char *value) {
    PRINT_INFO("Setting record: key='%s', value='%s'", key, value);
    
    // Build request
    uint8_t request[8192];
    size_t request_len = build_set_request(key, value, request, sizeof(request));
    
    if (request_len == 0) {
        return -1;
    }
    
    // Send RPC request
    uint8_t *response = NULL;
    size_t response_len = 0;
    
    int status = rpc_client_call(client, FUNC_ID_DATASTORE_SET,
                                 request, request_len,
                                 &response, &response_len, 5000);
    
    if (status != RPC_STATUS_SUCCESS) {
        PRINT_ERROR("RPC call failed with status: %d", status);
        return -1;
    }
    
    // Parse response
    parse_set_response(response, response_len);
    
    free(response);
    return 0;
}

static int handle_get(rpc_client_t *client, const char *key) {
    PRINT_INFO("Getting record: key='%s'", key);
    
    // Build request
    uint8_t request[8192];
    size_t request_len = build_get_request(key, request, sizeof(request));
    
    if (request_len == 0) {
        return -1;
    }
    
    // Send RPC request
    uint8_t *response = NULL;
    size_t response_len = 0;
    
    int status = rpc_client_call(client, FUNC_ID_DATASTORE_GET,
                                 request, request_len,
                                 &response, &response_len, 5000);
    
    if (status != RPC_STATUS_SUCCESS) {
        PRINT_ERROR("RPC call failed with status: %d", status);
        return -1;
    }
    
    // Parse response
    parse_get_response(response, response_len);
    
    free(response);
    return 0;
}

static int handle_unset(rpc_client_t *client, const char *key) {
    PRINT_INFO("Deleting record: key='%s'", key);
    
    // Build request
    uint8_t request[8192];
    size_t request_len = build_unset_request(key, request, sizeof(request));
    
    if (request_len == 0) {
        return -1;
    }
    
    // Send RPC request
    uint8_t *response = NULL;
    size_t response_len = 0;
    
    int status = rpc_client_call(client, FUNC_ID_DATASTORE_UNSET,
                                 request, request_len,
                                 &response, &response_len, 5000);
    
    if (status != RPC_STATUS_SUCCESS) {
        PRINT_ERROR("RPC call failed with status: %d", status);
        return -1;
    }
    
    // Parse response
    parse_unset_response(response, response_len);
    
    free(response);
    return 0;
}

static int handle_list(rpc_client_t *client) {
    PRINT_INFO("Listing all keys...");
    
    // Build request (empty for now)
    uint8_t request[8192];
    size_t request_len = build_list_request(request, sizeof(request));
    
    // Send RPC request
    uint8_t *response = NULL;
    size_t response_len = 0;
    
    int status = rpc_client_call(client, FUNC_ID_DATASTORE_LIST,
                                 request, request_len,
                                 &response, &response_len, 5000);
    
    if (status != RPC_STATUS_SUCCESS) {
        PRINT_ERROR("RPC call failed with status: %d", status);
        return -1;
    }
    
    // Parse response
    parse_list_response(response, response_len);
    
    free(response);
    return 0;
}

// ============================================================================
// INTERACTIVE MODE
// ============================================================================

static void print_help(void) {
    printf("\n");
    printf("Available commands:\n");
    printf("  set <key> <value>  - Set a key-value pair\n");
    printf("  get <key>          - Get value by key\n");
    printf("  unset <key>        - Delete a key\n");
    printf("  list               - List all keys\n");
    printf("  help               - Show this help\n");
    printf("  quit               - Exit interactive mode\n");
    printf("\n");
}

static int interactive_mode(rpc_client_t *client) {
    printf("\n");
    printf("╔════════════════════════════════════════════════════╗\n");
    printf("║   Roole Datastore - Interactive Mode              ║\n");
    printf("╚════════════════════════════════════════════════════╝\n");
    
    print_help();
    
    char line[4096];
    
    while (1) {
        printf(COLOR_CYAN "roole> " COLOR_RESET);
        fflush(stdout);
        
        if (!fgets(line, sizeof(line), stdin)) {
            break;
        }
        
        // Remove newline
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') {
            line[len - 1] = '\0';
        }
        
        // Skip empty lines
        if (strlen(line) == 0) {
            continue;
        }
        
        // Parse command
        char cmd[64];
        char arg1[1024];
        char arg2[2048];
        
        int args = sscanf(line, "%63s %1023s %2047[^\n]", cmd, arg1, arg2);
        
        if (args < 1) {
            continue;
        }
        
        if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0) {
            PRINT_INFO("Goodbye!");
            break;
        } else if (strcmp(cmd, "help") == 0) {
            print_help();
        } else if (strcmp(cmd, "set") == 0) {
            if (args < 3) {
                PRINT_ERROR("Usage: set <key> <value>");
            } else {
                handle_set(client, arg1, arg2);
            }
        } else if (strcmp(cmd, "get") == 0) {
            if (args < 2) {
                PRINT_ERROR("Usage: get <key>");
            } else {
                handle_get(client, arg1);
            }
        } else if (strcmp(cmd, "unset") == 0 || strcmp(cmd, "delete") == 0) {
            if (args < 2) {
                PRINT_ERROR("Usage: unset <key>");
            } else {
                handle_unset(client, arg1);
            }
        } else if (strcmp(cmd, "list") == 0 || strcmp(cmd, "ls") == 0) {
            handle_list(client);
        } else {
            PRINT_ERROR("Unknown command: %s (type 'help' for available commands)", cmd);
        }
        
        printf("\n");
    }
    
    return 0;
}

// ============================================================================
// MAIN
// ============================================================================

static void print_usage(const char *prog) {
    fprintf(stderr, "Usage: %s <host> <port> <operation> [args]\n", prog);
    fprintf(stderr, "\n");
    fprintf(stderr, "Operations:\n");
    fprintf(stderr, "  set <key> <value>    Set a key-value pair\n");
    fprintf(stderr, "  get <key>            Get value by key\n");
    fprintf(stderr, "  unset <key>          Delete a key\n");
    fprintf(stderr, "  list                 List all keys\n");
    fprintf(stderr, "  interactive          Enter interactive mode\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Examples:\n");
    fprintf(stderr, "  %s 127.0.0.1 8081 set mykey myvalue\n", prog);
    fprintf(stderr, "  %s 127.0.0.1 8081 get mykey\n", prog);
    fprintf(stderr, "  %s 127.0.0.1 8081 list\n", prog);
    fprintf(stderr, "  %s 127.0.0.1 8081 interactive\n", prog);
    fprintf(stderr, "\n");
}

int main(int argc, char **argv) {
    if (argc < 4) {
        print_usage(argv[0]);
        return 1;
    }
    
    const char *host = argv[1];
    uint16_t port = atoi(argv[2]);
    const char *operation = argv[3];
    
    // Validate port
    if (port == 0) {
        PRINT_ERROR("Invalid port: %s", argv[2]);
        return 1;
    }
    
    // Connect to ingress server
    PRINT_INFO("Connecting to %s:%u...", host, port);
    
    rpc_client_t *client = rpc_client_connect(host, port, 
                                              RPC_CHANNEL_INGRESS, 8192);
    
    if (!client) {
        PRINT_ERROR("Failed to connect to %s:%u", host, port);
        PRINT_INFO("Make sure the router node is running with ingress enabled");
        return 1;
    }
    
    PRINT_SUCCESS("Connected to datastore");
    printf("\n");
    
    int result = 0;
    
    // Handle operations
    if (strcmp(operation, "set") == 0) {
        if (argc < 6) {
            PRINT_ERROR("Usage: %s %s %s set <key> <value>", argv[0], host, argv[2]);
            result = 1;
        } else {
            result = handle_set(client, argv[4], argv[5]);
        }
    } else if (strcmp(operation, "get") == 0) {
        if (argc < 5) {
            PRINT_ERROR("Usage: %s %s %s get <key>", argv[0], host, argv[2]);
            result = 1;
        } else {
            result = handle_get(client, argv[4]);
        }
    } else if (strcmp(operation, "unset") == 0 || strcmp(operation, "delete") == 0) {
        if (argc < 5) {
            PRINT_ERROR("Usage: %s %s %s unset <key>", argv[0], host, argv[2]);
            result = 1;
        } else {
            result = handle_unset(client, argv[4]);
        }
    } else if (strcmp(operation, "list") == 0 || strcmp(operation, "ls") == 0) {
        result = handle_list(client);
    } else if (strcmp(operation, "interactive") == 0 || strcmp(operation, "i") == 0) {
        result = interactive_mode(client);
    } else {
        PRINT_ERROR("Unknown operation: %s", operation);
        print_usage(argv[0]);
        result = 1;
    }
    
    // Cleanup
    rpc_client_close(client);
    
    return result;
}
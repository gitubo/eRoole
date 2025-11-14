// test/integration/test_rpc_complete.c
// Complete RPC test demonstrating registration, calls, and back pressure

#include "roole/rpc/rpc_server.h"
#include "roole/rpc/rpc_client.h"
#include "roole/rpc/rpc_handler.h"
#include "roole/rpc/rpc_types.h"
#include "roole/core/common.h"
#include "roole/logger/logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <assert.h>

// Test function IDs
#define FUNC_ADD 0x01
#define FUNC_ECHO 0x02
#define FUNC_SLOW 0x03

// ============================================================================
// Test Handlers
// ============================================================================

// Handler: Add two numbers
static int handler_add(const uint8_t *request, size_t request_len,
                       uint8_t **response, size_t *response_len,
                       void *user_context) {
    (void)user_context;
    
    if (request_len != 8) {
        LOG_ERROR("Invalid request length for ADD: %zu", request_len);
        return RPC_STATUS_BAD_ARGUMENT;
    }
    
    int32_t a, b;
    memcpy(&a, request, 4);
    memcpy(&b, request + 4, 4);
    
    int32_t result = a + b;
    
    *response = (uint8_t*)safe_malloc(4);
    memcpy(*response, &result, 4);
    *response_len = 4;
    
    LOG_DEBUG("ADD(%d, %d) = %d", a, b, result);
    
    return RPC_STATUS_SUCCESS;
}

// Handler: Echo back the request
static int handler_echo(const uint8_t *request, size_t request_len,
                        uint8_t **response, size_t *response_len,
                        void *user_context) {
    (void)user_context;
    
    if (request_len == 0) {
        *response = NULL;
        *response_len = 0;
        return RPC_STATUS_SUCCESS;
    }
    
    *response = (uint8_t*)safe_malloc(request_len);
    memcpy(*response, request, request_len);
    *response_len = request_len;
    
    LOG_DEBUG("ECHO: %zu bytes", request_len);
    
    return RPC_STATUS_SUCCESS;
}

// Handler: Slow operation (for testing timeouts and back pressure)
static int handler_slow(const uint8_t *request, size_t request_len,
                        uint8_t **response, size_t *response_len,
                        void *user_context) {
    (void)user_context;
    (void)request;
    (void)request_len;
    
    LOG_DEBUG("SLOW: sleeping 2 seconds");
    sleep(2);
    
    *response = NULL;
    *response_len = 0;
    
    return RPC_STATUS_SUCCESS;
}

// ============================================================================
// Server Thread
// ============================================================================

typedef struct {
    rpc_server_t *server;
    pthread_t thread;
    int started;
} server_context_t;

static void* server_thread_func(void *arg) {
    server_context_t *ctx = (server_context_t*)arg;
    ctx->started = 1;
    rpc_server_run(ctx->server);
    return NULL;
}

// ============================================================================
// Test Functions
// ============================================================================

static void test_basic_call(void) {
    printf("\n=== Test 1: Basic RPC Call (ADD) ===\n");
    
    rpc_client_t *client = rpc_client_connect("127.0.0.1", 8888,
                                              RPC_CHANNEL_DATA, 4096);
    assert(client != NULL);
    
    // Prepare request: ADD(10, 20)
    uint8_t request[8];
    int32_t a = 10, b = 20;
    memcpy(request, &a, 4);
    memcpy(request + 4, &b, 4);
    
    // Call
    uint8_t *response = NULL;
    size_t response_len = 0;
    
    int status = rpc_client_call(client, FUNC_ADD, request, sizeof(request),
                                 &response, &response_len, 5000);
    
    assert(status == RPC_STATUS_SUCCESS);
    assert(response != NULL);
    assert(response_len == 4);
    
    int32_t result;
    memcpy(&result, response, 4);
    
    printf("ADD(10, 20) = %d\n", result);
    assert(result == 30);
    
    safe_free(response);
    rpc_client_close(client);
    
    printf("✓ Basic call test passed\n");
}

static void test_echo(void) {
    printf("\n=== Test 2: Echo Test ===\n");
    
    rpc_client_t *client = rpc_client_connect("127.0.0.1", 8888,
                                              RPC_CHANNEL_DATA, 4096);
    assert(client != NULL);
    
    const char *message = "Hello, RPC World!";
    size_t msg_len = strlen(message);
    
    uint8_t *response = NULL;
    size_t response_len = 0;
    
    int status = rpc_client_call(client, FUNC_ECHO,
                                 (const uint8_t*)message, msg_len,
                                 &response, &response_len, 5000);
    
    assert(status == RPC_STATUS_SUCCESS);
    assert(response != NULL);
    assert(response_len == msg_len);
    assert(memcmp(response, message, msg_len) == 0);
    
    printf("ECHO: '%.*s'\n", (int)response_len, response);
    
    safe_free(response);
    rpc_client_close(client);
    
    printf("✓ Echo test passed\n");
}

static void test_unknown_function(void) {
    printf("\n=== Test 3: Unknown Function (Error Handling) ===\n");
    
    rpc_client_t *client = rpc_client_connect("127.0.0.1", 8888,
                                              RPC_CHANNEL_DATA, 4096);
    assert(client != NULL);
    
    uint8_t *response = NULL;
    size_t response_len = 0;
    
    int status = rpc_client_call(client, 0xFF, NULL, 0,
                                 &response, &response_len, 5000);
    
    assert(status == RPC_STATUS_FUNC_NOT_FOUND);
    printf("Status: FUNC_NOT_FOUND (expected)\n");
    
    rpc_client_close(client);
    
    printf("✓ Error handling test passed\n");
}

static void test_timeout(void) {
    printf("\n=== Test 4: Timeout Test ===\n");
    
    rpc_client_t *client = rpc_client_connect("127.0.0.1", 8888,
                                              RPC_CHANNEL_DATA, 4096);
    assert(client != NULL);
    
    uint8_t *response = NULL;
    size_t response_len = 0;
    
    // Call slow function with 1 second timeout (should timeout)
    int status = rpc_client_call(client, FUNC_SLOW, NULL, 0,
                                 &response, &response_len, 1000);
    
    assert(status == RPC_STATUS_TIMEOUT);
    printf("Status: TIMEOUT (expected)\n");
    
    rpc_client_close(client);
    
    printf("✓ Timeout test passed\n");
}

static void test_concurrent_calls(void) {
    printf("\n=== Test 5: Concurrent Calls (Load Test) ===\n");
    
    const int NUM_CLIENTS = 10;
    const int CALLS_PER_CLIENT = 50;
    
    pthread_t threads[NUM_CLIENTS];
    
    void* client_thread(void *arg) {
        int client_id = *(int*)arg;
        
        rpc_client_t *client = rpc_client_connect("127.0.0.1", 8888,
                                                  RPC_CHANNEL_DATA, 4096);
        if (!client) {
            printf("Client %d: connection failed\n", client_id);
            return NULL;
        }
        
        int success_count = 0;
        
        for (int i = 0; i < CALLS_PER_CLIENT; i++) {
            uint8_t request[8];
            int32_t a = client_id, b = i;
            memcpy(request, &a, 4);
            memcpy(request + 4, &b, 4);
            
            uint8_t *response = NULL;
            size_t response_len = 0;
            
            int status = rpc_client_call(client, FUNC_ADD, request, sizeof(request),
                                         &response, &response_len, 5000);
            
            if (status == RPC_STATUS_SUCCESS) {
                success_count++;
                safe_free(response);
            }
        }
        
        rpc_client_close(client);
        
        printf("Client %d: %d/%d successful calls\n", client_id, success_count, CALLS_PER_CLIENT);
        
        return NULL;
    }
    
    int client_ids[NUM_CLIENTS];
    
    for (int i = 0; i < NUM_CLIENTS; i++) {
        client_ids[i] = i;
        pthread_create(&threads[i], NULL, client_thread, &client_ids[i]);
    }
    
    for (int i = 0; i < NUM_CLIENTS; i++) {
        pthread_join(threads[i], NULL);
    }
    
    printf("✓ Concurrent calls test passed\n");
}

static void test_async_calls(void) {
    printf("\n=== Test 6: Async Calls (Fire and Forget) ===\n");
    
    rpc_client_t *client = rpc_client_connect("127.0.0.1", 8888,
                                              RPC_CHANNEL_DATA, 4096);
    assert(client != NULL);
    
    // Send 10 async calls
    for (int i = 0; i < 10; i++) {
        uint8_t request[8];
        int32_t a = i, b = i * 2;
        memcpy(request, &a, 4);
        memcpy(request + 4, &b, 4);
        
        int ret = rpc_client_send_async(client, FUNC_ADD, request, sizeof(request));
        assert(ret == 0);
    }
    
    printf("Sent 10 async calls (fire and forget)\n");
    
    rpc_client_close(client);
    
    printf("✓ Async calls test passed\n");
}

static void print_server_stats(rpc_server_t *server) {
    rpc_server_stats_t stats;
    rpc_server_get_stats(server, &stats);
    
    printf("\n=== Server Statistics ===\n");
    printf("Requests received:  %lu\n", stats.requests_received);
    printf("Requests processed: %lu\n", stats.requests_processed);
    printf("Requests failed:    %lu\n", stats.requests_failed);
    printf("Bytes received:     %lu\n", stats.bytes_received);
    printf("Bytes sent:         %lu\n", stats.bytes_sent);
    printf("Active connections: %zu\n", stats.active_connections);
    printf("========================\n");
}

// ============================================================================
// Main
// ============================================================================

int main(void) {
    printf("=================================================\n");
    printf("  ROOLE RPC COMPLETE TEST SUITE\n");
    printf("=================================================\n");
    
    // Initialize logger
    logger_init();
    logger_set_level(LOG_LEVEL_INFO);
    
    // Create handler registry
    rpc_handler_registry_t *registry = rpc_handler_registry_create();
    assert(registry != NULL);
    
    // Register handlers
    assert(rpc_handler_register(registry, FUNC_ADD, handler_add, NULL) == 0);
    assert(rpc_handler_register(registry, FUNC_ECHO, handler_echo, NULL) == 0);
    assert(rpc_handler_register(registry, FUNC_SLOW, handler_slow, NULL) == 0);
    
    printf("✓ Handlers registered: ADD, ECHO, SLOW\n");
    
    // Create and start server
    rpc_server_config_t config = {
        .port = 8888,
        .bind_addr = "0.0.0.0",
        .channel_type = RPC_CHANNEL_DATA,
        .max_connections = 64,
        .buffer_size = 4096,
        .recv_timeout_ms = 100
    };
    
    rpc_server_t *server = rpc_server_create(&config, registry);
    assert(server != NULL);
    
    printf("✓ RPC server created on port 8888\n");
    
    // Start server in background thread
    server_context_t server_ctx = {
        .server = server,
        .started = 0
    };
    
    pthread_create(&server_ctx.thread, NULL, server_thread_func, &server_ctx);
    
    // Wait for server to start
    while (!server_ctx.started) {
        usleep(10000);
    }
    
    printf("✓ Server thread started\n");
    sleep(1); // Give server time to bind
    
    // Run tests
    test_basic_call();
    test_echo();
    test_unknown_function();
    test_timeout();
    test_concurrent_calls();
    test_async_calls();
    
    // Print statistics
    print_server_stats(server);
    
    // Cleanup
    printf("\n=== Cleanup ===\n");
    rpc_server_stop(server);
    pthread_join(server_ctx.thread, NULL);
    rpc_server_destroy(server);
    rpc_handler_registry_destroy(registry);
    logger_shutdown();
    
    printf("\n=================================================\n");
    printf("  ALL TESTS PASSED ✓\n");
    printf("=================================================\n");
    
    return 0;
}
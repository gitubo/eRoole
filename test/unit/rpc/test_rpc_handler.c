#include "roole/rpc/rpc_handler.h"
#include "roole/core/common.h"
#include "roole/logger/logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>

// Test handlers
static int test_handler_1(const uint8_t *req, size_t req_len,
                         uint8_t **resp, size_t *resp_len, void *ctx) {
    (void)req; (void)req_len; (void)ctx;
    *resp = (uint8_t*)malloc(4);
    memcpy(*resp, "OK-1", 4);
    *resp_len = 4;
    return RPC_STATUS_SUCCESS;
}

static int test_handler_2(const uint8_t *req, size_t req_len,
                         uint8_t **resp, size_t *resp_len, void *ctx) {
    (void)req; (void)req_len; (void)ctx;
    *resp = (uint8_t*)malloc(4);
    memcpy(*resp, "OK-2", 4);
    *resp_len = 4;
    return RPC_STATUS_SUCCESS;
}

static int test_handler_error(const uint8_t *req, size_t req_len,
                              uint8_t **resp, size_t *resp_len, void *ctx) {
    (void)req; (void)req_len; (void)resp; (void)resp_len; (void)ctx;
    return RPC_STATUS_INTERNAL_ERROR;
}

// Test 1: Basic registration and lookup
void test_basic_registration(void) {
    printf("Test 1: Basic registration and lookup... ");
    
    rpc_handler_registry_t *reg = rpc_handler_registry_create();
    assert(reg != NULL);
    
    // Register handlers
    assert(rpc_handler_register(reg, 0x01, test_handler_1, NULL) == 0);
    assert(rpc_handler_register(reg, 0x02, test_handler_2, NULL) == 0);
    
    // Lookup handlers
    void *ctx = NULL;
    rpc_handler_fn h1 = rpc_handler_lookup(reg, 0x01, &ctx);
    assert(h1 == test_handler_1);
    assert(ctx == NULL);
    
    rpc_handler_fn h2 = rpc_handler_lookup(reg, 0x02, &ctx);
    assert(h2 == test_handler_2);
    
    // Lookup non-existent handler
    rpc_handler_fn h3 = rpc_handler_lookup(reg, 0xFF, &ctx);
    assert(h3 == NULL);
    
    rpc_handler_registry_destroy(reg);
    
    printf("✓\n");
}

// Test 2: Unregister handler
void test_unregister(void) {
    printf("Test 2: Unregister handler... ");
    
    rpc_handler_registry_t *reg = rpc_handler_registry_create();
    assert(reg != NULL);
    
    assert(rpc_handler_register(reg, 0x10, test_handler_1, NULL) == 0);
    
    // Verify registered
    assert(rpc_handler_lookup(reg, 0x10, NULL) == test_handler_1);
    
    // Unregister
    assert(rpc_handler_unregister(reg, 0x10) == 0);
    
    // Verify removed
    assert(rpc_handler_lookup(reg, 0x10, NULL) == NULL);
    
    // Unregister non-existent (should fail gracefully)
    assert(rpc_handler_unregister(reg, 0xFF) == -1);
    
    rpc_handler_registry_destroy(reg);
    
    printf("✓\n");
}

// Test 3: User context
void test_user_context(void) {
    printf("Test 3: User context passing... ");
    
    rpc_handler_registry_t *reg = rpc_handler_registry_create();
    assert(reg != NULL);
    
    int context_data = 42;
    
    assert(rpc_handler_register(reg, 0x20, test_handler_1, &context_data) == 0);
    
    void *retrieved_ctx = NULL;
    rpc_handler_fn handler = rpc_handler_lookup(reg, 0x20, &retrieved_ctx);
    
    assert(handler == test_handler_1);
    assert(retrieved_ctx == &context_data);
    assert(*(int*)retrieved_ctx == 42);
    
    rpc_handler_registry_destroy(reg);
    
    printf("✓\n");
}

// Test 4: Update existing handler
void test_update_handler(void) {
    printf("Test 4: Update existing handler... ");
    
    rpc_handler_registry_t *reg = rpc_handler_registry_create();
    assert(reg != NULL);
    
    // Register initial handler
    assert(rpc_handler_register(reg, 0x30, test_handler_1, NULL) == 0);
    assert(rpc_handler_lookup(reg, 0x30, NULL) == test_handler_1);
    
    // Update with new handler
    assert(rpc_handler_register(reg, 0x30, test_handler_2, NULL) == 0);
    assert(rpc_handler_lookup(reg, 0x30, NULL) == test_handler_2);
    
    rpc_handler_registry_destroy(reg);
    
    printf("✓\n");
}

// Test 5: Multiple handlers
void test_multiple_handlers(void) {
    printf("Test 5: Multiple handlers... ");
    
    rpc_handler_registry_t *reg = rpc_handler_registry_create();
    assert(reg != NULL);
    
    // Register many handlers
    for (uint8_t i = 0; i < 50; i++) {
        rpc_handler_fn handler = (i % 2 == 0) ? test_handler_1 : test_handler_2;
        assert(rpc_handler_register(reg, i, handler, NULL) == 0);
    }
    
    // Verify all handlers
    for (uint8_t i = 0; i < 50; i++) {
        rpc_handler_fn expected = (i % 2 == 0) ? test_handler_1 : test_handler_2;
        rpc_handler_fn actual = rpc_handler_lookup(reg, i, NULL);
        assert(actual == expected);
    }
    
    rpc_handler_registry_destroy(reg);
    
    printf("✓\n");
}

// Test 6: Thread safety (concurrent lookups)
typedef struct {
    rpc_handler_registry_t *registry;
    int thread_id;
    int success;
} thread_test_data_t;

static void* concurrent_lookup_thread(void *arg) {
    thread_test_data_t *data = (thread_test_data_t*)arg;
    
    for (int i = 0; i < 10000; i++) {
        uint8_t func_id = (uint8_t)(i % 50);
        rpc_handler_fn handler = rpc_handler_lookup(data->registry, func_id, NULL);
        
        if (handler == NULL) {
            data->success = 0;
            return NULL;
        }
    }
    
    data->success = 1;
    return NULL;
}

void test_concurrent_access(void) {
    printf("Test 6: Concurrent access... ");
    
    rpc_handler_registry_t *reg = rpc_handler_registry_create();
    assert(reg != NULL);
    
    // Register handlers
    for (uint8_t i = 0; i < 50; i++) {
        assert(rpc_handler_register(reg, i, test_handler_1, NULL) == 0);
    }
    
    // Create multiple threads
    const int NUM_THREADS = 8;
    pthread_t threads[NUM_THREADS];
    thread_test_data_t thread_data[NUM_THREADS];
    
    for (int i = 0; i < NUM_THREADS; i++) {
        thread_data[i].registry = reg;
        thread_data[i].thread_id = i;
        thread_data[i].success = 0;
        pthread_create(&threads[i], NULL, concurrent_lookup_thread, &thread_data[i]);
    }
    
    // Wait for all threads
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
        assert(thread_data[i].success == 1);
    }
    
    rpc_handler_registry_destroy(reg);
    
    printf("✓\n");
}

// Test 7: Handler invocation
void test_handler_invocation(void) {
    printf("Test 7: Handler invocation... ");
    
    rpc_handler_registry_t *reg = rpc_handler_registry_create();
    assert(reg != NULL);
    
    assert(rpc_handler_register(reg, 0x01, test_handler_1, NULL) == 0);
    assert(rpc_handler_register(reg, 0x02, test_handler_error, NULL) == 0);
    
    // Invoke successful handler
    rpc_handler_fn h1 = rpc_handler_lookup(reg, 0x01, NULL);
    assert(h1 != NULL);
    
    uint8_t *resp = NULL;
    size_t resp_len = 0;
    int status = h1(NULL, 0, &resp, &resp_len, NULL);
    
    assert(status == RPC_STATUS_SUCCESS);
    assert(resp != NULL);
    assert(resp_len == 4);
    assert(memcmp(resp, "OK-1", 4) == 0);
    free(resp);
    
    // Invoke error handler
    rpc_handler_fn h2 = rpc_handler_lookup(reg, 0x02, NULL);
    assert(h2 != NULL);
    
    resp = NULL;
    status = h2(NULL, 0, &resp, &resp_len, NULL);
    assert(status == RPC_STATUS_INTERNAL_ERROR);
    
    rpc_handler_registry_destroy(reg);
    
    printf("✓\n");
}

// Test 8: Edge cases
void test_edge_cases(void) {
    printf("Test 8: Edge cases... ");
    
    // NULL registry
    assert(rpc_handler_register(NULL, 0x01, test_handler_1, NULL) == -1);
    assert(rpc_handler_lookup(NULL, 0x01, NULL) == NULL);
    assert(rpc_handler_unregister(NULL, 0x01) == -1);
    
    rpc_handler_registry_t *reg = rpc_handler_registry_create();
    assert(reg != NULL);
    
    // NULL handler
    assert(rpc_handler_register(reg, 0x01, NULL, NULL) == -1);
    
    // Destroy NULL
    rpc_handler_registry_destroy(NULL);  // Should not crash
    
    rpc_handler_registry_destroy(reg);
    
    printf("✓\n");
}

int main(void) {
    printf("====================================\n");
    printf("  RPC Handler Registry Unit Tests\n");
    printf("====================================\n\n");
    
    logger_init();
    logger_set_level(LOG_LEVEL_WARN);
    
    test_basic_registration();
    test_unregister();
    test_user_context();
    test_update_handler();
    test_multiple_handlers();
    test_concurrent_access();
    test_handler_invocation();
    test_edge_cases();
    
    logger_shutdown();
    
    printf("\n====================================\n");
    printf("  All tests passed ✓\n");
    printf("====================================\n");
    
    return 0;
}
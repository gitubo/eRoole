// test/tools/dag_register.c - DAG Registration Tool

#define _POSIX_C_SOURCE 200809L

#include "roole/rpc/rpc_client.h"
#include "roole/rpc/rpc_types.h"
#include "roole/dag/dag.h"
#include "roole/core/common.h"
#include "roole/logger/logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <endian.h>

// ============================================================================
// BUILD SIMPLE LINEAR DAG
// ============================================================================

static dag_t* build_test_dag(uint32_t dag_id, const char *name, uint32_t num_steps) {
    dag_t *dag = (dag_t*)safe_calloc(1, sizeof(dag_t));
    if (!dag) {
        LOG_ERROR("Failed to allocate DAG");
        return NULL;
    }
    
    dag->dag_id = dag_id;
    safe_strncpy(dag->name, name, MAX_DAG_NAME);
    dag->version = 1;
    dag->step_count = num_steps;
    dag->created_at_ms = time_now_ms();
    dag->updated_at_ms = dag->created_at_ms;
    
    // Create linear pipeline: step0 -> step1 -> ... -> stepN
    for (uint32_t i = 0; i < num_steps; i++) {
        dag_step_t *step = &dag->steps[i];
        
        step->step_id = i;
        snprintf(step->name, MAX_STEP_NAME, "step_%u", i);
        snprintf(step->function_name, MAX_STEP_NAME, "process_step_%u", i);
        
        // Set dependencies (each step depends on previous, except first)
        if (i == 0) {
            step->dependency_count = 0;
        } else {
            step->dependency_count = 1;
            step->dependencies[0] = i - 1;  // Depends on previous step
        }
        
        step->timeout_ms = 5000;  // 5 second timeout
        step->max_retries = 3;
        
        // No config data for simple test
        step->config_data = NULL;
        step->config_len = 0;
    }
    
    LOG_INFO("Built test DAG: id=%u, name='%s', steps=%u", 
             dag_id, name, num_steps);
    
    return dag;
}

// ============================================================================
// MAIN
// ============================================================================

int main(int argc, char *argv[]) {
    if (argc != 6) {
        fprintf(stderr, "Usage: %s <router_host> <ingress_port> <dag_id> <dag_name> <num_steps>\n", argv[0]);
        fprintf(stderr, "\n");
        fprintf(stderr, "Arguments:\n");
        fprintf(stderr, "  router_host    - Router hostname/IP (e.g., 127.0.0.1)\n");
        fprintf(stderr, "  ingress_port   - Router ingress port (e.g., 8081)\n");
        fprintf(stderr, "  dag_id         - Unique DAG identifier (e.g., 1)\n");
        fprintf(stderr, "  dag_name       - Human-readable DAG name\n");
        fprintf(stderr, "  num_steps      - Number of pipeline steps (1-10)\n");
        fprintf(stderr, "\n");
        fprintf(stderr, "Example:\n");
        fprintf(stderr, "  %s 127.0.0.1 8081 1 \"test_pipeline\" 3\n", argv[0]);
        fprintf(stderr, "\n");
        return 1;
    }

    const char *host = argv[1];
    uint16_t port = (uint16_t)atoi(argv[2]);
    uint32_t dag_id = (uint32_t)atoi(argv[3]);
    const char *dag_name = argv[4];
    uint32_t num_steps = (uint32_t)atoi(argv[5]);

    if (num_steps == 0 || num_steps > 10) {
        fprintf(stderr, "ERROR: num_steps must be between 1 and 10\n");
        return 1;
    }

    // Initialize logger
    logger_init();
    logger_set_level(LOG_LEVEL_INFO);
    logger_set_context(0, "dag-register", "tool");

    LOG_INFO("========================================");
    LOG_INFO("DAG Registration Tool");
    LOG_INFO("  Target: %s:%u", host, port);
    LOG_INFO("  DAG ID: %u", dag_id);
    LOG_INFO("  DAG Name: %s", dag_name);
    LOG_INFO("  Steps: %u", num_steps);
    LOG_INFO("========================================");

    // Build test DAG
    dag_t *dag = build_test_dag(dag_id, dag_name, num_steps);
    if (!dag) {
        LOG_ERROR("Failed to build test DAG");
        logger_shutdown();
        return 1;
    }

    // Validate DAG
    if (dag_validate(dag) != RESULT_OK) {
        LOG_ERROR("DAG validation failed");
        safe_free(dag);
        logger_shutdown();
        return 1;
    }

    LOG_INFO("✓ DAG validated successfully");

    // Serialize DAG
    uint8_t *serialized_buffer = (uint8_t*)safe_malloc(65536);  // 64KB buffer
    if (!serialized_buffer) {
        LOG_ERROR("Failed to allocate serialization buffer");
        safe_free(dag);
        logger_shutdown();
        return 1;
    }

    size_t serialized_len = dag_serialize(dag, serialized_buffer, 65536);
    if (serialized_len == 0) {
        LOG_ERROR("Failed to serialize DAG");
        safe_free(serialized_buffer);
        safe_free(dag);
        logger_shutdown();
        return 1;
    }

    LOG_INFO("✓ DAG serialized: %zu bytes", serialized_len);

    // Connect to router's INGRESS channel
    LOG_INFO("Connecting to router ingress at %s:%u...", host, port);
    
    rpc_client_t *client = rpc_client_connect(
        host,
        port,
        RPC_CHANNEL_INGRESS,
        65536  // 64KB buffer
    );

    if (!client) {
        LOG_ERROR("Failed to connect to router INGRESS channel");
        safe_free(serialized_buffer);
        safe_free(dag);
        logger_shutdown();
        return 1;
    }

    LOG_INFO("✓ Connected to router");

    // Send ADD_DAG RPC request
    LOG_INFO("Sending DAG registration request...");

    uint8_t *response = NULL;
    size_t response_len = 0;

    int rpc_status = rpc_client_call(
        client,
        FUNC_ID_ADD_DAG,
        serialized_buffer,
        serialized_len,
        &response,
        &response_len,
        10000  // 10 second timeout
    );

    // Cleanup
    safe_free(serialized_buffer);
    safe_free(dag);

    // Check result
    if (rpc_status != RPC_STATUS_SUCCESS) {
        LOG_ERROR("DAG registration failed with status: %d", rpc_status);
        
        if (response && response_len > 0) {
            LOG_ERROR("Error response: %.*s", (int)response_len, (char*)response);
        }
        
        if (response) safe_free(response);
        rpc_client_close(client);
        logger_shutdown();
        return 1;
    }

    LOG_INFO("========================================");
    LOG_INFO("✓ DAG REGISTERED SUCCESSFULLY");
    LOG_INFO("  DAG ID: %u", dag_id);
    LOG_INFO("  Name: %s", dag_name);
    LOG_INFO("  Steps: %u", num_steps);
    LOG_INFO("========================================");

    // Cleanup
    if (response) safe_free(response);
    rpc_client_close(client);
    logger_shutdown();

    return 0;
}
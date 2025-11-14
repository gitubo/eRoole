#define _POSIX_C_SOURCE 200809L

#include "roole/node/node_handlers.h"
#include "roole/node/node_state.h"
#include "roole/core/common.h"
#include "roole/core/service_registry.h"
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <endian.h>

// ============================================================================
// HANDLER: Submit Message for Processing
// Request: [dag_id: 4 bytes][message: variable]
// Response: [exec_id: 8 bytes][status: 1 byte]
// ============================================================================

int handle_submit_message(const uint8_t *request, size_t request_len,
                          uint8_t **response, size_t *response_len,
                          void *user_context) {
    if (!request || request_len < 4 || !response || !response_len || !user_context) {
        LOG_ERROR("Invalid submit_message parameters");
        return RPC_STATUS_BAD_ARGUMENT;
    }
    
    node_state_t *state = (node_state_t*)user_context;
    
    // Parse DAG ID (network byte order)
    rule_id_t dag_id;
    memcpy(&dag_id, request, 4);
    dag_id = ntohl(dag_id);
    
    const uint8_t *message = request + 4;
    size_t message_len = request_len - 4;
    
    if (message_len == 0 || message_len > MAX_MESSAGE_SIZE) {
        LOG_ERROR("Invalid message length: %zu", message_len);
        return RPC_STATUS_BAD_ARGUMENT;
    }
    
    LOG_DEBUG("Submit message: dag_id=%u, msg_len=%zu", dag_id, message_len);
    
    // Check if DAG exists
    dag_catalog_t *catalog = node_state_get_dag_catalog(state);
    dag_t *dag = dag_catalog_get(catalog, dag_id);
    if (!dag) {
        LOG_ERROR("DAG %u not found in catalog", dag_id);
        return RPC_STATUS_BAD_ARGUMENT;
    }
    dag_catalog_release(catalog);
    
    // Select worker for execution
    peer_pool_t *pool = node_state_get_peer_pool(state);
    node_id_t worker_id = peer_pool_select_least_loaded(pool);
    
    if (worker_id == 0) {
        // No workers available, execute locally if capable
        const node_capabilities_t *caps = node_state_get_capabilities(state);
        if (caps->can_execute) {
            worker_id = state->identity.node_id;  // Self
            LOG_DEBUG("No workers available, executing locally");
        } else {
            LOG_ERROR("No workers available and node cannot execute");
            return RPC_STATUS_INTERNAL_ERROR;
        }
    } else {
        LOG_DEBUG("Selected worker %u for execution", worker_id);
    }
    
    // Add to execution tracker
    execution_tracker_t *tracker = node_state_get_exec_tracker(state);
    execution_id_t exec_id = execution_tracker_add(
        tracker, dag_id, worker_id, message, message_len, 3  // max 3 retries
    );
    
    if (exec_id == 0) {
        LOG_ERROR("Execution tracker full");
        return RPC_STATUS_INTERNAL_ERROR;
    }
    
    LOG_INFO("Created execution: exec_id=%lu, dag_id=%u, worker=%u", 
            exec_id, dag_id, worker_id);
    
    // Queue message for processing
    message_queue_t *queue = node_state_get_message_queue(state);
    message_t msg;
    memset(&msg, 0, sizeof(msg));
    
    msg.exec_id = exec_id;
    msg.dag_id = dag_id;
    msg.sender_id = 0;  // Client request
    msg.received_at_ms = time_now_ms();
    memcpy(msg.message_data, message, message_len);
    msg.message_len = message_len;
    
    int queue_result = message_queue_push(queue, &msg);
    if (queue_result != RESULT_OK) {
        LOG_ERROR("Failed to queue message: %d", queue_result);
        execution_tracker_remove(tracker, exec_id);
        return RPC_STATUS_INTERNAL_ERROR;
    }
    
    LOG_DEBUG("Message queued successfully (exec_id=%lu)", exec_id);
    
    // Build response: [exec_id: 8 bytes][status: 1 byte]
    *response = (uint8_t*)safe_malloc(9);
    if (!*response) {
        return RPC_STATUS_INTERNAL_ERROR;
    }
    
    uint64_t exec_id_net = htobe64(exec_id);
    memcpy(*response, &exec_id_net, 8);
    (*response)[8] = (uint8_t)EXEC_STATUS_PENDING;
    *response_len = 9;
    
    return RPC_STATUS_SUCCESS;
}

// ============================================================================
// HANDLER: Get Execution Status
// Request: [exec_id: 8 bytes]
// Response: [status: 1 byte][optional: result data]
// ============================================================================

int handle_get_execution_status(const uint8_t *request, size_t request_len,
                                uint8_t **response, size_t *response_len,
                                void *user_context) {
    if (!request || request_len < 8 || !response || !response_len || !user_context) {
        return RPC_STATUS_BAD_ARGUMENT;
    }
    
    node_state_t *state = (node_state_t*)user_context;
    
    // Parse execution ID
    execution_id_t exec_id;
    memcpy(&exec_id, request, 8);
    exec_id = be64toh(exec_id);
    
    LOG_DEBUG("Get status: exec_id=%lu", exec_id);
    
    // Get execution record
    execution_tracker_t *tracker = node_state_get_exec_tracker(state);
    execution_record_t *record = execution_tracker_get(tracker, exec_id);
    
    if (!record) {
        LOG_WARN("Execution %lu not found", exec_id);
        execution_tracker_release(tracker);
        return RPC_STATUS_BAD_ARGUMENT;
    }
    
    execution_status_t status = record->status;
    execution_tracker_release(tracker);
    
    // Build response: [status: 1 byte]
    *response = (uint8_t*)safe_malloc(1);
    if (!*response) {
        return RPC_STATUS_INTERNAL_ERROR;
    }
    
    (*response)[0] = (uint8_t)status;
    *response_len = 1;
    
    LOG_DEBUG("Execution %lu status: %d", exec_id, status);
    
    return RPC_STATUS_SUCCESS;
}

// ============================================================================
// HANDLER: List DAGs
// Request: empty
// Response: [count: 4 bytes][dag_id_1: 4][dag_id_2: 4]...
// ============================================================================

int handle_list_dags(const uint8_t *request, size_t request_len,
                    uint8_t **response, size_t *response_len,
                    void *user_context) {
    (void)request;
    (void)request_len;
    
    if (!response || !response_len || !user_context) {
        return RPC_STATUS_BAD_ARGUMENT;
    }
    
    node_state_t *state = (node_state_t*)user_context;
    
    // Get DAG list from catalog
    dag_catalog_t *catalog = node_state_get_dag_catalog(state);
    
    rule_id_t dag_ids[MAX_DAGS];
    size_t count = dag_catalog_list(catalog, dag_ids, MAX_DAGS);
    
    LOG_DEBUG("Listing %zu DAGs", count);
    
    // Build response: [count: 4][dag_id_1: 4][dag_id_2: 4]...
    size_t response_size = 4 + (count * 4);
    *response = (uint8_t*)safe_malloc(response_size);
    if (!*response) {
        return RPC_STATUS_INTERNAL_ERROR;
    }
    
    uint32_t count_net = htonl((uint32_t)count);
    memcpy(*response, &count_net, 4);
    
    for (size_t i = 0; i < count; i++) {
        uint32_t dag_id_net = htonl(dag_ids[i]);
        memcpy(*response + 4 + (i * 4), &dag_id_net, 4);
    }
    
    *response_len = response_size;
    
    return RPC_STATUS_SUCCESS;
}

// ============================================================================
// HANDLER: Add DAG
// Request: [serialized DAG]
// Response: [ack: 1 byte]
// ============================================================================

int handle_add_dag(const uint8_t *request, size_t request_len,
                  uint8_t **response, size_t *response_len,
                  void *user_context) {
    if (!request || request_len == 0 || !response || !response_len || !user_context) {
        return RPC_STATUS_BAD_ARGUMENT;
    }
    
    node_state_t *state = (node_state_t*)user_context;
    
    // Deserialize DAG
    dag_t dag;
    int result = dag_deserialize(request, request_len, &dag);
    
    if (result != RESULT_OK) {
        LOG_ERROR("Failed to deserialize DAG");
        return RPC_STATUS_BAD_ARGUMENT;
    }
    
    LOG_INFO("Adding DAG %u '%s' (%zu steps)", dag.dag_id, dag.name, dag.step_count);
    
    // Validate DAG
    if (dag_validate(&dag) != RESULT_OK) {
        LOG_ERROR("DAG validation failed");
        return RPC_STATUS_BAD_ARGUMENT;
    }
    
    // Add to catalog
    dag_catalog_t *catalog = node_state_get_dag_catalog(state);
    result = dag_catalog_add(catalog, &dag);
    
    if (result != RESULT_OK) {
        LOG_ERROR("Failed to add DAG to catalog: %d", result);
        return RPC_STATUS_INTERNAL_ERROR;
    }
    
    // ACK response
    *response = (uint8_t*)safe_malloc(1);
    if (!*response) {
        return RPC_STATUS_INTERNAL_ERROR;
    }
    
    (*response)[0] = 1;  // ACK
    *response_len = 1;
    
    LOG_INFO("DAG %u added successfully", dag.dag_id);
    
    return RPC_STATUS_SUCCESS;
}
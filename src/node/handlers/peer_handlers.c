#define _POSIX_C_SOURCE 200809L

#include "roole/node/node_handlers.h"
#include "roole/node/node_state.h"
#include "roole/core/common.h"
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

// ============================================================================
// HANDLER: Process Message (from router to worker)
// Request: [exec_id: 8][dag_id: 4][sender_id: 2][message: variable]
// Response: [ack: 1 byte]
// ============================================================================

int handle_process_message(const uint8_t *request, size_t request_len,
                           uint8_t **response, size_t *response_len,
                           void *user_context) {
    if (!request || request_len < 14 || !response || !response_len || !user_context) {
        LOG_ERROR("Invalid process_message parameters");
        return RPC_STATUS_BAD_ARGUMENT;
    }
    
    node_state_t *state = (node_state_t*)user_context;
    
    // Parse request
    execution_id_t exec_id;
    rule_id_t dag_id;
    node_id_t sender_id;
    
    memcpy(&exec_id, request, 8);
    exec_id = be64toh(exec_id);
    
    memcpy(&dag_id, request + 8, 4);
    dag_id = ntohl(dag_id);
    
    memcpy(&sender_id, request + 12, 2);
    sender_id = ntohs(sender_id);
    
    const uint8_t *message = request + 14;
    size_t message_len = request_len - 14;
    
    if (message_len > MAX_MESSAGE_SIZE) {
        LOG_ERROR("Message too large: %zu bytes", message_len);
        return RPC_STATUS_BAD_ARGUMENT;
    }
    
    LOG_DEBUG("Process message: exec_id=%lu, dag_id=%u, sender=%u, msg_len=%zu",
             exec_id, dag_id, sender_id, message_len);
    
    // Queue for execution
    message_queue_t *queue = node_state_get_message_queue(state);
    message_t msg;
    memset(&msg, 0, sizeof(msg));
    
    msg.exec_id = exec_id;
    msg.dag_id = dag_id;
    msg.sender_id = sender_id;
    msg.received_at_ms = time_now_ms();
    memcpy(msg.message_data, message, message_len);
    msg.message_len = message_len;
    
    int result = message_queue_push(queue, &msg);
    if (result != RESULT_OK) {
        LOG_ERROR("Failed to queue message: %d", result);
        return RPC_STATUS_INTERNAL_ERROR;
    }
    
    LOG_DEBUG("Message queued successfully (exec_id=%lu)", exec_id);
    
    // ACK response
    *response = (uint8_t*)safe_malloc(1);
    if (!*response) {
        return RPC_STATUS_INTERNAL_ERROR;
    }
    
    (*response)[0] = 1;  // ACK
    *response_len = 1;
    
    return RPC_STATUS_SUCCESS;
}

// ============================================================================
// HANDLER: Execution Update (status update from worker)
// Request: [exec_id: 8][status: 1]
// Response: [ack: 1 byte]
// ============================================================================

int handle_execution_update(const uint8_t *request, size_t request_len,
                            uint8_t **response, size_t *response_len,
                            void *user_context) {
    if (!request || request_len < 9 || !response || !response_len || !user_context) {
        return RPC_STATUS_BAD_ARGUMENT;
    }
    
    node_state_t *state = (node_state_t*)user_context;
    
    // Parse request
    execution_id_t exec_id;
    execution_status_t status;
    
    memcpy(&exec_id, request, 8);
    exec_id = be64toh(exec_id);
    
    status = (execution_status_t)request[8];
    
    LOG_DEBUG("Execution update: exec_id=%lu, status=%d", exec_id, status);
    
    // Update execution tracker
    execution_tracker_t *tracker = node_state_get_exec_tracker(state);
    int result = execution_tracker_update_status(tracker, exec_id, status);
    
    if (result != RESULT_OK) {
        LOG_WARN("Failed to update execution %lu: %d", exec_id, result);
        return RPC_STATUS_BAD_ARGUMENT;
    }
    
    // ACK response
    *response = (uint8_t*)safe_malloc(1);
    if (!*response) {
        return RPC_STATUS_INTERNAL_ERROR;
    }
    
    (*response)[0] = 1;  // ACK
    *response_len = 1;
    
    return RPC_STATUS_SUCCESS;
}

// ============================================================================
// HANDLER: Sync Catalog (sync DAG between nodes)
// Request: [serialized DAG]
// Response: [ack: 1 byte]
// ============================================================================

int handle_sync_catalog(const uint8_t *request, size_t request_len,
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
        LOG_ERROR("Failed to deserialize DAG for sync");
        return RPC_STATUS_BAD_ARGUMENT;
    }
    
    LOG_INFO("Syncing DAG %u '%s' from peer", dag.dag_id, dag.name);
    
    // Add or update in catalog
    dag_catalog_t *catalog = node_state_get_dag_catalog(state);
    
    // Try update first
    result = dag_catalog_update(catalog, &dag);
    if (result == RESULT_ERR_NOTFOUND) {
        // Doesn't exist, add it
        result = dag_catalog_add(catalog, &dag);
    }
    
    if (result != RESULT_OK) {
        LOG_ERROR("Failed to sync DAG: %d", result);
        return RPC_STATUS_INTERNAL_ERROR;
    }
    
    // ACK response
    *response = (uint8_t*)safe_malloc(1);
    if (!*response) {
        return RPC_STATUS_INTERNAL_ERROR;
    }
    
    (*response)[0] = 1;  // ACK
    *response_len = 1;
    
    LOG_INFO("DAG %u synced successfully", dag.dag_id);
    
    return RPC_STATUS_SUCCESS;
}
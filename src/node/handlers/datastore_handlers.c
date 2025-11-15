// src/node/handlers/datastore_handlers.c
// RPC handlers for datastore operations (replacing DAG handlers)

#define _POSIX_C_SOURCE 200809L

#include "roole/node/node_handlers.h"
#include "roole/node/node_state.h"
#include "roole/datastore/datastore.h"
#include "roole/core/common.h"
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <endian.h>

// ============================================================================
// HANDLER: Set Record
// Request: [key_len: 2][key: variable][value_len: 4][value: variable]
// Response: [ack: 1 byte][version: 8 bytes]
// ============================================================================

int handle_datastore_set(const uint8_t *request, size_t request_len,
                         uint8_t **response, size_t *response_len,
                         void *user_context) {
    if (!request || request_len < 6 || !response || !response_len || !user_context) {
        LOG_ERROR("Invalid datastore_set parameters");
        return RPC_STATUS_BAD_ARGUMENT;
    }
    
    node_state_t *state = (node_state_t*)user_context;
    const uint8_t *ptr = request;
    const uint8_t *end = request + request_len;
    
    // Parse key length and key
    if (ptr + 2 > end) return RPC_STATUS_BAD_ARGUMENT;
    uint16_t key_len_net;
    memcpy(&key_len_net, ptr, 2);
    uint16_t key_len = ntohs(key_len_net);
    ptr += 2;
    
    if (key_len >= MAX_KEY_LEN || ptr + key_len > end) {
        LOG_ERROR("Invalid key length: %u", key_len);
        return RPC_STATUS_BAD_ARGUMENT;
    }
    
    char key[MAX_KEY_LEN];
    memcpy(key, ptr, key_len);
    key[key_len] = '\0';
    ptr += key_len;
    
    // Parse value length and value
    if (ptr + 4 > end) return RPC_STATUS_BAD_ARGUMENT;
    uint32_t value_len_net;
    memcpy(&value_len_net, ptr, 4);
    uint32_t value_len = ntohl(value_len_net);
    ptr += 4;
    
    if (value_len == 0 || value_len > MAX_VALUE_SIZE || ptr + value_len > end) {
        LOG_ERROR("Invalid value length: %u", value_len);
        return RPC_STATUS_BAD_ARGUMENT;
    }
    
    const uint8_t *value = ptr;
    
    LOG_DEBUG("Set record: key=%s, value_len=%u", key, value_len);
    
    // Get datastore
    datastore_t *store = node_state_get_datastore(state);
    if (!store) {
        LOG_ERROR("Datastore not initialized");
        return RPC_STATUS_INTERNAL_ERROR;
    }
    
    // Set record
    int result = datastore_set(store, key, value, value_len, state->identity.node_id);
    
    if (result != RESULT_OK) {
        LOG_ERROR("Failed to set record: %d", result);
        return RPC_STATUS_INTERNAL_ERROR;
    }
    
    // Get version for response
    datastore_record_t *record = datastore_get(store, key);
    uint64_t version = record ? record->version : 0;
    datastore_release(store);
    
    // Build response: [ack: 1][version: 8]
    *response = (uint8_t*)safe_malloc(9);
    if (!*response) {
        return RPC_STATUS_INTERNAL_ERROR;
    }
    
    (*response)[0] = 1;  // ACK
    uint64_t version_net = htobe64(version);
    memcpy(*response + 1, &version_net, 8);
    *response_len = 9;
    
    LOG_INFO("Record set successfully: key=%s, version=%lu", key, version);
    
    return RPC_STATUS_SUCCESS;
}

// ============================================================================
// HANDLER: Get Record
// Request: [key_len: 2][key: variable]
// Response: [found: 1][value_len: 4][value: variable][version: 8]
// ============================================================================

int handle_datastore_get(const uint8_t *request, size_t request_len,
                         uint8_t **response, size_t *response_len,
                         void *user_context) {
    if (!request || request_len < 2 || !response || !response_len || !user_context) {
        return RPC_STATUS_BAD_ARGUMENT;
    }
    
    node_state_t *state = (node_state_t*)user_context;
    const uint8_t *ptr = request;
    const uint8_t *end = request + request_len;
    
    // Parse key
    if (ptr + 2 > end) return RPC_STATUS_BAD_ARGUMENT;
    uint16_t key_len_net;
    memcpy(&key_len_net, ptr, 2);
    uint16_t key_len = ntohs(key_len_net);
    ptr += 2;
    
    if (key_len >= MAX_KEY_LEN || ptr + key_len > end) {
        return RPC_STATUS_BAD_ARGUMENT;
    }
    
    char key[MAX_KEY_LEN];
    memcpy(key, ptr, key_len);
    key[key_len] = '\0';
    
    LOG_DEBUG("Get record: key=%s", key);
    
    // Get datastore
    datastore_t *store = node_state_get_datastore(state);
    if (!store) {
        return RPC_STATUS_INTERNAL_ERROR;
    }
    
    // Get record
    datastore_record_t *record = datastore_get(store, key);
    
    if (!record) {
        datastore_release(store);
        
        // Record not found - return empty response
        *response = (uint8_t*)safe_malloc(1);
        if (!*response) {
            return RPC_STATUS_INTERNAL_ERROR;
        }
        
        (*response)[0] = 0;  // Not found
        *response_len = 1;
        
        LOG_DEBUG("Record not found: key=%s", key);
        return RPC_STATUS_SUCCESS;
    }
    
    // Build response: [found: 1][value_len: 4][value][version: 8]
    size_t response_size = 1 + 4 + record->value_len + 8;
    *response = (uint8_t*)safe_malloc(response_size);
    
    if (!*response) {
        datastore_release(store);
        return RPC_STATUS_INTERNAL_ERROR;
    }
    
    uint8_t *resp_ptr = *response;
    
    // Found flag
    *resp_ptr++ = 1;
    
    // Value length
    uint32_t value_len_net = htonl((uint32_t)record->value_len);
    memcpy(resp_ptr, &value_len_net, 4);
    resp_ptr += 4;
    
    // Value
    memcpy(resp_ptr, record->value, record->value_len);
    resp_ptr += record->value_len;
    
    // Version
    uint64_t version_net = htobe64(record->version);
    memcpy(resp_ptr, &version_net, 8);
    resp_ptr += 8;
    
    *response_len = response_size;
    
    datastore_release(store);
    
    LOG_DEBUG("Record retrieved: key=%s, len=%zu, version=%lu",
             key, record->value_len, record->version);
    
    return RPC_STATUS_SUCCESS;
}

// ============================================================================
// HANDLER: Unset (Delete) Record
// Request: [key_len: 2][key: variable]
// Response: [ack: 1 byte]
// ============================================================================

int handle_datastore_unset(const uint8_t *request, size_t request_len,
                           uint8_t **response, size_t *response_len,
                           void *user_context) {
    if (!request || request_len < 2 || !response || !response_len || !user_context) {
        return RPC_STATUS_BAD_ARGUMENT;
    }
    
    node_state_t *state = (node_state_t*)user_context;
    const uint8_t *ptr = request;
    const uint8_t *end = request + request_len;
    
    // Parse key
    if (ptr + 2 > end) return RPC_STATUS_BAD_ARGUMENT;
    uint16_t key_len_net;
    memcpy(&key_len_net, ptr, 2);
    uint16_t key_len = ntohs(key_len_net);
    ptr += 2;
    
    if (key_len >= MAX_KEY_LEN || ptr + key_len > end) {
        return RPC_STATUS_BAD_ARGUMENT;
    }
    
    char key[MAX_KEY_LEN];
    memcpy(key, ptr, key_len);
    key[key_len] = '\0';
    
    LOG_DEBUG("Unset record: key=%s", key);
    
    // Get datastore
    datastore_t *store = node_state_get_datastore(state);
    if (!store) {
        return RPC_STATUS_INTERNAL_ERROR;
    }
    
    // Unset record
    int result = datastore_unset(store, key);
    
    if (result == RESULT_ERR_NOTFOUND) {
        LOG_DEBUG("Record not found for unset: %s", key);
        // Still return success (idempotent)
    } else if (result != RESULT_OK) {
        LOG_ERROR("Failed to unset record: %d", result);
        return RPC_STATUS_INTERNAL_ERROR;
    }
    
    // ACK response
    *response = (uint8_t*)safe_malloc(1);
    if (!*response) {
        return RPC_STATUS_INTERNAL_ERROR;
    }
    
    (*response)[0] = 1;  // ACK
    *response_len = 1;
    
    LOG_INFO("Record unset successfully: key=%s", key);
    
    return RPC_STATUS_SUCCESS;
}

// ============================================================================
// HANDLER: List Keys
// Request: empty or [prefix_len: 2][prefix: variable]
// Response: [count: 4][key1_len: 2][key1]...[keyN_len: 2][keyN]
// ============================================================================

int handle_datastore_list_keys(const uint8_t *request, size_t request_len,
                                uint8_t **response, size_t *response_len,
                                void *user_context) {
    if (!response || !response_len || !user_context) {
        return RPC_STATUS_BAD_ARGUMENT;
    }
    
    node_state_t *state = (node_state_t*)user_context;
    
    // Optional: parse prefix filter (if request_len > 0)
    // For now, list all keys
    
    LOG_DEBUG("List keys");
    
    // Get datastore
    datastore_t *store = node_state_get_datastore(state);
    if (!store) {
        return RPC_STATUS_INTERNAL_ERROR;
    }
    
    // Get all keys
    char *keys[MAX_RECORDS];
    size_t count = datastore_list_keys(store, keys, MAX_RECORDS);
    
    LOG_DEBUG("Found %zu keys", count);
    
    // Calculate response size
    size_t response_size = 4;  // count
    for (size_t i = 0; i < count; i++) {
        response_size += 2 + strlen(keys[i]);
    }
    
    *response = (uint8_t*)safe_malloc(response_size);
    if (!*response) {
        return RPC_STATUS_INTERNAL_ERROR;
    }
    
    uint8_t *ptr = *response;
    
    // Write count
    uint32_t count_net = htonl((uint32_t)count);
    memcpy(ptr, &count_net, 4);
    ptr += 4;
    
    // Write each key
    for (size_t i = 0; i < count; i++) {
        uint16_t key_len = (uint16_t)strlen(keys[i]);
        uint16_t key_len_net = htons(key_len);
        
        memcpy(ptr, &key_len_net, 2);
        ptr += 2;
        
        memcpy(ptr, keys[i], key_len);
        ptr += key_len;
    }
    
    *response_len = response_size;
    
    LOG_DEBUG("Returned %zu keys (%zu bytes)", count, response_size);
    
    return RPC_STATUS_SUCCESS;
}

// ============================================================================
// HANDLER: Sync Datastore (for gossip/replication)
// Request: [serialized records]
// Response: [ack: 1 byte][merged_count: 4]
// ============================================================================

int handle_datastore_sync(const uint8_t *request, size_t request_len,
                          uint8_t **response, size_t *response_len,
                          void *user_context) {
    if (!request || request_len == 0 || !response || !response_len || !user_context) {
        return RPC_STATUS_BAD_ARGUMENT;
    }
    
    node_state_t *state = (node_state_t*)user_context;
    
    LOG_DEBUG("Syncing datastore: %zu bytes", request_len);
    
    // Deserialize records
    datastore_record_t *records = NULL;
    size_t count = 0;
    
    int result = datastore_deserialize_records(request, request_len, &records, &count);
    
    if (result != RESULT_OK) {
        LOG_ERROR("Failed to deserialize records for sync: %d", result);
        return RPC_STATUS_BAD_ARGUMENT;
    }
    
    LOG_INFO("Received %zu records for sync", count);
    
    // Get datastore
    datastore_t *store = node_state_get_datastore(state);
    if (!store) {
        // Free records
        for (size_t i = 0; i < count; i++) {
            datastore_free_record(&records[i]);
        }
        safe_free(records);
        return RPC_STATUS_INTERNAL_ERROR;
    }
    
    // Merge each record
    size_t merged = 0;
    for (size_t i = 0; i < count; i++) {
        if (datastore_merge_record(store, &records[i]) == RESULT_OK) {
            merged++;
        }
        datastore_free_record(&records[i]);
    }
    safe_free(records);
    
    LOG_INFO("Merged %zu/%zu records", merged, count);
    
    // Build response: [ack: 1][merged_count: 4]
    *response = (uint8_t*)safe_malloc(5);
    if (!*response) {
        return RPC_STATUS_INTERNAL_ERROR;
    }
    
    (*response)[0] = 1;  // ACK
    uint32_t merged_net = htonl((uint32_t)merged);
    memcpy(*response + 1, &merged_net, 4);
    *response_len = 5;
    
    return RPC_STATUS_SUCCESS;
}
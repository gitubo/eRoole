// src/node/handlers/raft_datastore_handlers.c
// RPC handlers for Raft-backed strongly consistent datastore

#define _POSIX_C_SOURCE 200809L

#include "roole/node/node_handlers.h"
#include "roole/node/node_state.h"
#include "roole/raft/raft_datastore.h"
#include "roole/raft/raft_state.h"
#include "roole/core/common.h"
#include "roole/logger/logger.h"
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <endian.h>

// ============================================================================
// HANDLER: Raft KV Set (Linearizable Write)
// Request: [key_len: 2][key: variable][value_len: 4][value: variable]
// Response: [success: 1][index: 8][term: 8]
// ============================================================================

int handle_raft_kv_set(const uint8_t *request,
                       size_t request_len,
                       uint8_t **response,
                       size_t *response_len,
                       void *user_context) {
    if (!request || request_len < 6 || !response || !response_len || !user_context) {
        LOG_ERROR("Invalid raft_kv_set parameters");
        return RPC_STATUS_BAD_ARGUMENT;
    }
    
    node_state_t *state = (node_state_t*)user_context;
    const uint8_t *ptr = request;
    const uint8_t *end = request + request_len;
    
    // Check if we have Raft datastore
    if (!state->raft_datastore) {
        LOG_ERROR("Raft datastore not initialized");
        return RPC_STATUS_INTERNAL_ERROR;
    }
    
    // Parse key length and key
    if (ptr + 2 > end) return RPC_STATUS_BAD_ARGUMENT;
    uint16_t key_len_net;
    memcpy(&key_len_net, ptr, 2);
    uint16_t key_len = ntohs(key_len_net);
    ptr += 2;
    
    if (key_len >= RAFT_KV_MAX_KEY_LEN || ptr + key_len > end) {
        LOG_ERROR("Invalid key length: %u", key_len);
        return RPC_STATUS_BAD_ARGUMENT;
    }
    
    char key[RAFT_KV_MAX_KEY_LEN];
    memcpy(key, ptr, key_len);
    key[key_len] = '\0';
    ptr += key_len;
    
    // Parse value length and value
    if (ptr + 4 > end) return RPC_STATUS_BAD_ARGUMENT;
    uint32_t value_len_net;
    memcpy(&value_len_net, ptr, 4);
    uint32_t value_len = ntohl(value_len_net);
    ptr += 4;
    
    if (value_len == 0 || value_len > RAFT_KV_MAX_VALUE_SIZE || ptr + value_len > end) {
        LOG_ERROR("Invalid value length: %u", value_len);
        return RPC_STATUS_BAD_ARGUMENT;
    }
    
    const uint8_t *value = ptr;
    
    LOG_DEBUG("Raft KV SET: key=%s, value_len=%u", key, value_len);
    
    // Submit to Raft with 5 second timeout
    int result = raft_datastore_set(state->raft_datastore, key, value, value_len, 5000);
    
    // Build response: [success: 1][index: 8][term: 8]
    *response = (uint8_t*)safe_malloc(17);
    if (!*response) {
        return RPC_STATUS_INTERNAL_ERROR;
    }
    
    uint8_t *resp_ptr = *response;
    
    // Success flag
    *resp_ptr++ = (result == RESULT_OK) ? 1 : 0;
    
    // Log index and term (0 if failed)
    uint64_t index = 0;
    uint64_t term = 0;
    
    if (result == RESULT_OK) {
        // Get current commit index and term from Raft
        if (state->raft_state) {
            index = raft_get_commit_index(state->raft_state);
            term = raft_get_term(state->raft_state);
        }
    }
    
    uint64_t index_net = htobe64(index);
    memcpy(resp_ptr, &index_net, 8);
    resp_ptr += 8;
    
    uint64_t term_net = htobe64(term);
    memcpy(resp_ptr, &term_net, 8);
    resp_ptr += 8;
    
    *response_len = 17;
    
    if (result == RESULT_OK) {
        LOG_INFO("Raft KV SET succeeded: key=%s, index=%lu, term=%lu", key, index, term);
        return RPC_STATUS_SUCCESS;
    } else {
        LOG_WARN("Raft KV SET failed: key=%s, result=%d", key, result);
        return RPC_STATUS_SUCCESS; // Still return success, but with failure flag in payload
    }
}

// ============================================================================
// HANDLER: Raft KV Get (Linearizable Read)
// Request: [key_len: 2][key: variable]
// Response: [found: 1][value_len: 4][value: variable]
// ============================================================================

int handle_raft_kv_get(const uint8_t *request,
                       size_t request_len,
                       uint8_t **response,
                       size_t *response_len,
                       void *user_context) {
    if (!request || request_len < 2 || !response || !response_len || !user_context) {
        return RPC_STATUS_BAD_ARGUMENT;
    }
    
    node_state_t *state = (node_state_t*)user_context;
    const uint8_t *ptr = request;
    const uint8_t *end = request + request_len;
    
    if (!state->raft_datastore) {
        LOG_ERROR("Raft datastore not initialized");
        return RPC_STATUS_INTERNAL_ERROR;
    }
    
    // Parse key
    if (ptr + 2 > end) return RPC_STATUS_BAD_ARGUMENT;
    uint16_t key_len_net;
    memcpy(&key_len_net, ptr, 2);
    uint16_t key_len = ntohs(key_len_net);
    ptr += 2;
    
    if (key_len >= RAFT_KV_MAX_KEY_LEN || ptr + key_len > end) {
        return RPC_STATUS_BAD_ARGUMENT;
    }
    
    char key[RAFT_KV_MAX_KEY_LEN];
    memcpy(key, ptr, key_len);
    key[key_len] = '\0';
    
    LOG_DEBUG("Raft KV GET: key=%s", key);
    
    // Get from Raft datastore (linearizable read with 5s timeout)
    uint8_t *value = NULL;
    size_t value_len = 0;
    int result = raft_datastore_get(state->raft_datastore, key, &value, &value_len, 5000);
    
    if (result == RESULT_OK && value) {
        // Build response: [found: 1][value_len: 4][value]
        size_t response_size = 1 + 4 + value_len;
        *response = (uint8_t*)safe_malloc(response_size);
        
        if (!*response) {
            safe_free(value);
            return RPC_STATUS_INTERNAL_ERROR;
        }
        
        uint8_t *resp_ptr = *response;
        
        // Found flag
        *resp_ptr++ = 1;
        
        // Value length
        uint32_t value_len_net = htonl((uint32_t)value_len);
        memcpy(resp_ptr, &value_len_net, 4);
        resp_ptr += 4;
        
        // Value
        memcpy(resp_ptr, value, value_len);
        resp_ptr += value_len;
        
        *response_len = response_size;
        
        safe_free(value);
        
        LOG_DEBUG("Raft KV GET succeeded: key=%s, len=%zu", key, value_len);
        return RPC_STATUS_SUCCESS;
    } else {
        // Not found - return empty response
        *response = (uint8_t*)safe_malloc(1);
        if (!*response) {
            return RPC_STATUS_INTERNAL_ERROR;
        }
        
        (*response)[0] = 0; // Not found
        *response_len = 1;
        
        LOG_DEBUG("Raft KV GET: key=%s not found", key);
        return RPC_STATUS_SUCCESS;
    }
}

// ============================================================================
// HANDLER: Raft KV Unset (Linearizable Delete)
// Request: [key_len: 2][key: variable]
// Response: [success: 1]
// ============================================================================

int handle_raft_kv_unset(const uint8_t *request,
                         size_t request_len,
                         uint8_t **response,
                         size_t *response_len,
                         void *user_context) {
    if (!request || request_len < 2 || !response || !response_len || !user_context) {
        return RPC_STATUS_BAD_ARGUMENT;
    }
    
    node_state_t *state = (node_state_t*)user_context;
    const uint8_t *ptr = request;
    const uint8_t *end = request + request_len;
    
    if (!state->raft_datastore) {
        LOG_ERROR("Raft datastore not initialized");
        return RPC_STATUS_INTERNAL_ERROR;
    }
    
    // Parse key
    if (ptr + 2 > end) return RPC_STATUS_BAD_ARGUMENT;
    uint16_t key_len_net;
    memcpy(&key_len_net, ptr, 2);
    uint16_t key_len = ntohs(key_len_net);
    ptr += 2;
    
    if (key_len >= RAFT_KV_MAX_KEY_LEN || ptr + key_len > end) {
        return RPC_STATUS_BAD_ARGUMENT;
    }
    
    char key[RAFT_KV_MAX_KEY_LEN];
    memcpy(key, ptr, key_len);
    key[key_len] = '\0';
    
    LOG_DEBUG("Raft KV UNSET: key=%s", key);
    
    // Unset from Raft datastore (with 5s timeout)
    int result = raft_datastore_unset(state->raft_datastore, key, 5000);
    
    // Build response: [success: 1]
    *response = (uint8_t*)safe_malloc(1);
    if (!*response) {
        return RPC_STATUS_INTERNAL_ERROR;
    }
    
    (*response)[0] = (result == RESULT_OK || result == RESULT_ERR_NOTFOUND) ? 1 : 0;
    *response_len = 1;
    
    if (result == RESULT_OK || result == RESULT_ERR_NOTFOUND) {
        LOG_INFO("Raft KV UNSET succeeded: key=%s", key);
        return RPC_STATUS_SUCCESS;
    } else {
        LOG_WARN("Raft KV UNSET failed: key=%s, result=%d", key, result);
        return RPC_STATUS_SUCCESS; // Still return success, but with failure flag
    }
}

// ============================================================================
// HANDLER: Raft KV List Keys (Eventually Consistent)
// Request: empty or [prefix_len: 2][prefix: variable]
// Response: [count: 4][key1_len: 2][key1]...[keyN_len: 2][keyN]
// ============================================================================

int handle_raft_kv_list(const uint8_t *request,
                        size_t request_len,
                        uint8_t **response,
                        size_t *response_len,
                        void *user_context) {
    if (!response || !response_len || !user_context) {
        return RPC_STATUS_BAD_ARGUMENT;
    }
    
    node_state_t *state = (node_state_t*)user_context;
    
    if (!state->raft_datastore) {
        LOG_ERROR("Raft datastore not initialized");
        return RPC_STATUS_INTERNAL_ERROR;
    }
    
    // Optional: parse prefix filter (if request_len > 0)
    // For now, list all keys
    (void)request;
    (void)request_len;
    
    LOG_DEBUG("Raft KV LIST");
    
    // Get all keys
    char *keys[RAFT_KV_MAX_RECORDS];
    size_t count = raft_datastore_list_keys(state->raft_datastore, keys, RAFT_KV_MAX_RECORDS);
    
    LOG_DEBUG("Raft KV LIST: found %zu keys", count);
    
    // Calculate response size
    size_t response_size = 4; // count
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
    
    LOG_DEBUG("Raft KV LIST: returned %zu keys (%zu bytes)", count, response_size);
    
    return RPC_STATUS_SUCCESS;
}

// ============================================================================
// HANDLER: Raft Status (Get Raft Cluster State)
// Request: empty
// Response: [is_leader: 1][term: 8][commit_index: 8][leader_id: 2]
// ============================================================================

int handle_raft_status(const uint8_t *request,
                       size_t request_len,
                       uint8_t **response,
                       size_t *response_len,
                       void *user_context) {
    (void)request;
    (void)request_len;
    
    if (!response || !response_len || !user_context) {
        return RPC_STATUS_BAD_ARGUMENT;
    }
    
    node_state_t *state = (node_state_t*)user_context;
    
    if (!state->raft_state) {
        LOG_ERROR("Raft state not initialized");
        return RPC_STATUS_INTERNAL_ERROR;
    }
    
    // Get Raft status
    int is_leader = raft_is_leader(state->raft_state);
    uint64_t term = raft_get_term(state->raft_state);
    uint64_t commit_index = raft_get_commit_index(state->raft_state);
    node_id_t leader_id = raft_get_leader(state->raft_state);
    
    LOG_DEBUG("Raft STATUS: leader=%d term=%lu commit=%lu leader_id=%u",
              is_leader, term, commit_index, leader_id);
    
    // Build response: [is_leader: 1][term: 8][commit_index: 8][leader_id: 2]
    *response = (uint8_t*)safe_malloc(19);
    if (!*response) {
        return RPC_STATUS_INTERNAL_ERROR;
    }
    
    uint8_t *ptr = *response;
    
    // Is leader flag
    *ptr++ = is_leader ? 1 : 0;
    
    // Term
    uint64_t term_net = htobe64(term);
    memcpy(ptr, &term_net, 8);
    ptr += 8;
    
    // Commit index
    uint64_t commit_net = htobe64(commit_index);
    memcpy(ptr, &commit_net, 8);
    ptr += 8;
    
    // Leader ID
    uint16_t leader_net = htons(leader_id);
    memcpy(ptr, &leader_net, 2);
    ptr += 2;
    
    *response_len = 19;
    
    return RPC_STATUS_SUCCESS;
}
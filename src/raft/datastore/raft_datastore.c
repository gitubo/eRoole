// src/raft/datastore/raft_datastore.c
// Strongly consistent key-value store backed by Raft consensus
// This REPLACES the old eventually-consistent datastore

#define _POSIX_C_SOURCE 200809L

#include "roole/raft/raft_datastore.h"
#include "roole/raft/raft_state.h"
#include "roole/core/common.h"
#include "roole/logger/logger.h"
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <endian.h>

// ============================================================================
// INTERNAL HELPERS
// ============================================================================

static raft_kv_record_t* find_record(raft_datastore_t *store, const char *key) {
    for (size_t i = 0; i < store->capacity; i++) {
        if (store->records[i].active &&
            strcmp(store->records[i].key, key) == 0) {
            return &store->records[i];
        }
    }
    return NULL;
}

static raft_kv_record_t* find_free_slot(raft_datastore_t *store) {
    for (size_t i = 0; i < store->capacity; i++) {
        if (!store->records[i].active) {
            return &store->records[i];
        }
    }
    return NULL;
}

// ============================================================================
// LIFECYCLE
// ============================================================================

raft_datastore_t* raft_datastore_create(raft_state_t *raft_state, size_t capacity) {
    if (!raft_state || capacity == 0) {
        LOG_ERROR("Raft KV: Invalid create parameters");
        return NULL;
    }
    
    raft_datastore_t *store = safe_calloc(1, sizeof(raft_datastore_t));
    if (!store) {
        LOG_ERROR("Raft KV: Failed to allocate datastore");
        return NULL;
    }
    
    store->raft_state = raft_state;
    store->capacity = capacity;
    store->count = 0;
    
    store->records = safe_calloc(capacity, sizeof(raft_kv_record_t));
    if (!store->records) {
        LOG_ERROR("Raft KV: Failed to allocate records");
        safe_free(store);
        return NULL;
    }
    
    if (pthread_rwlock_init(&store->lock, NULL) != 0) {
        LOG_ERROR("Raft KV: Failed to init rwlock");
        safe_free(store->records);
        safe_free(store);
        return NULL;
    }
    
    if (pthread_mutex_init(&store->pending_lock, NULL) != 0) {
        LOG_ERROR("Raft KV: Failed to init pending lock");
        pthread_rwlock_destroy(&store->lock);
        safe_free(store->records);
        safe_free(store);
        return NULL;
    }
    
    if (pthread_cond_init(&store->commit_cond, NULL) != 0) {
        LOG_ERROR("Raft KV: Failed to init condition variable");
        pthread_mutex_destroy(&store->pending_lock);
        pthread_rwlock_destroy(&store->lock);
        safe_free(store->records);
        safe_free(store);
        return NULL;
    }
    
    LOG_INFO("Raft KV: Created strongly consistent datastore (capacity=%zu)", capacity);
    return store;
}

void raft_datastore_destroy(raft_datastore_t *store) {
    if (!store) return;
    
    pthread_rwlock_wrlock(&store->lock);
    
    // Free all record values
    for (size_t i = 0; i < store->capacity; i++) {
        if (store->records[i].active && store->records[i].value) {
            safe_free(store->records[i].value);
        }
    }
    
    safe_free(store->records);
    
    pthread_rwlock_unlock(&store->lock);
    
    pthread_cond_destroy(&store->commit_cond);
    pthread_mutex_destroy(&store->pending_lock);
    pthread_rwlock_destroy(&store->lock);
    
    safe_free(store);
    
    LOG_INFO("Raft KV: Datastore destroyed");
}

// ============================================================================
// COMMAND SERIALIZATION
// ============================================================================

// Format: [cmd_type:1][key_len:2][key][value_len:4][value]
size_t raft_cmd_serialize_set(const char *key,
                               const uint8_t *value,
                               size_t value_len,
                               uint8_t *buffer,
                               size_t buffer_size) {
    if (!key || !value || !buffer) {
        return 0;
    }
    
    size_t key_len = strlen(key);
    size_t required = 1 + 2 + key_len + 4 + value_len;
    
    if (buffer_size < required || key_len >= RAFT_KV_MAX_KEY_LEN) {
        return 0;
    }
    
    size_t offset = 0;
    
    // Command type
    buffer[offset++] = (uint8_t)RAFT_CMD_SET;
    
    // Key length and key
    uint16_t klen_net = htons((uint16_t)key_len);
    memcpy(buffer + offset, &klen_net, 2);
    offset += 2;
    
    memcpy(buffer + offset, key, key_len);
    offset += key_len;
    
    // Value length and value
    uint32_t vlen_net = htonl((uint32_t)value_len);
    memcpy(buffer + offset, &vlen_net, 4);
    offset += 4;
    
    memcpy(buffer + offset, value, value_len);
    offset += value_len;
    
    return offset;
}

// Format: [cmd_type:1][key_len:2][key]
size_t raft_cmd_serialize_unset(const char *key,
                                 uint8_t *buffer,
                                 size_t buffer_size) {
    if (!key || !buffer) {
        return 0;
    }
    
    size_t key_len = strlen(key);
    size_t required = 1 + 2 + key_len;
    
    if (buffer_size < required || key_len >= RAFT_KV_MAX_KEY_LEN) {
        return 0;
    }
    
    size_t offset = 0;
    
    // Command type
    buffer[offset++] = (uint8_t)RAFT_CMD_UNSET;
    
    // Key length and key
    uint16_t klen_net = htons((uint16_t)key_len);
    memcpy(buffer + offset, &klen_net, 2);
    offset += 2;
    
    memcpy(buffer + offset, key, key_len);
    offset += key_len;
    
    return offset;
}

// ============================================================================
// COMMAND EXECUTION (STATE MACHINE)
// ============================================================================

int raft_cmd_deserialize_and_execute(const uint8_t *data,
                                      size_t len,
                                      raft_datastore_t *store) {
    if (!data || len < 3 || !store) {
        return -1;
    }
    
    size_t offset = 0;
    
    // Command type
    raft_command_type_t cmd_type = (raft_command_type_t)data[offset++];
    
    // Key length
    uint16_t key_len_net;
    memcpy(&key_len_net, data + offset, 2);
    uint16_t key_len = ntohs(key_len_net);
    offset += 2;
    
    if (offset + key_len > len || key_len >= RAFT_KV_MAX_KEY_LEN) {
        return -1;
    }
    
    // Key
    char key[RAFT_KV_MAX_KEY_LEN];
    memcpy(key, data + offset, key_len);
    key[key_len] = '\0';
    offset += key_len;
    
    pthread_rwlock_wrlock(&store->lock);
    
    if (cmd_type == RAFT_CMD_SET) {
        // Value length
        if (offset + 4 > len) {
            pthread_rwlock_unlock(&store->lock);
            return -1;
        }
        
        uint32_t value_len_net;
        memcpy(&value_len_net, data + offset, 4);
        uint32_t value_len = ntohl(value_len_net);
        offset += 4;
        
        if (offset + value_len > len || value_len > RAFT_KV_MAX_VALUE_SIZE) {
            pthread_rwlock_unlock(&store->lock);
            return -1;
        }
        
        const uint8_t *value = data + offset;
        
        // Find or create record
        raft_kv_record_t *record = find_record(store, key);
        
        if (!record) {
            record = find_free_slot(store);
            if (!record) {
                pthread_rwlock_unlock(&store->lock);
                LOG_ERROR("Raft KV: Store full, cannot SET %s", key);
                return -1;
            }
            
            safe_strncpy(record->key, key, RAFT_KV_MAX_KEY_LEN);
            record->created_at_ms = time_now_ms();
            record->active = 1;
            store->count++;
        } else {
            // Free old value
            if (record->value) {
                safe_free(record->value);
            }
        }
        
        // Set new value
        record->value = safe_malloc(value_len);
        if (!record->value) {
            pthread_rwlock_unlock(&store->lock);
            return -1;
        }
        
        memcpy(record->value, value, value_len);
        record->value_len = value_len;
        record->version++;
        record->updated_at_ms = time_now_ms();
        
        store->total_sets++;
        
        LOG_INFO("Raft KV: SET %s (len=%u, version=%lu)", key, value_len, record->version);
        
    } else if (cmd_type == RAFT_CMD_UNSET) {
        // Find and delete record
        raft_kv_record_t *record = find_record(store, key);
        
        if (record) {
            if (record->value) {
                safe_free(record->value);
                record->value = NULL;
            }
            record->active = 0;
            store->count--;
            store->total_unsets++;
            
            LOG_INFO("Raft KV: UNSET %s", key);
        } else {
            LOG_DEBUG("Raft KV: UNSET %s (not found)", key);
        }
    } else {
        LOG_ERROR("Raft KV: Unknown command type %d", cmd_type);
        pthread_rwlock_unlock(&store->lock);
        return -1;
    }
    
    pthread_rwlock_unlock(&store->lock);
    
    // Signal any waiting threads
    pthread_cond_broadcast(&store->commit_cond);
    
    return 0;
}

// ============================================================================
// STATE MACHINE CALLBACKS
// ============================================================================

int raft_datastore_apply(const raft_log_entry_t *entry, void *user_data) {
    raft_datastore_t *store = (raft_datastore_t*)user_data;
    
    if (!entry || !store) {
        return -1;
    }
    
    // Skip no-op entries
    if (entry->type == RAFT_ENTRY_NOOP || entry->data_len == 0) {
        return 0;
    }
    
    LOG_DEBUG("Raft KV: Applying entry index=%lu term=%lu type=%d",
              entry->index, entry->term, entry->type);
    
    return raft_cmd_deserialize_and_execute(entry->data, entry->data_len, store);
}

int raft_datastore_snapshot(uint64_t last_included_index,
                             uint64_t last_included_term,
                             uint8_t **out_data,
                             size_t *out_len,
                             void *user_data) {
    raft_datastore_t *store = (raft_datastore_t*)user_data;
    
    if (!store || !out_data || !out_len) {
        return -1;
    }
    
    LOG_INFO("Raft KV: Creating snapshot (index=%lu, term=%lu)",
             last_included_index, last_included_term);
    
    // TODO: Implement snapshot creation
    // For now, return empty snapshot
    *out_data = NULL;
    *out_len = 0;
    
    return 0;
}

int raft_datastore_restore(const uint8_t *data,
                            size_t len,
                            uint64_t last_included_index,
                            uint64_t last_included_term,
                            void *user_data) {
    raft_datastore_t *store = (raft_datastore_t*)user_data;
    
    if (!store) {
        return -1;
    }
    
    LOG_INFO("Raft KV: Restoring from snapshot (index=%lu, term=%lu, size=%zu)",
             last_included_index, last_included_term, len);
    
    // TODO: Implement snapshot restoration
    
    return 0;
}

// ============================================================================
// CLIENT OPERATIONS (LINEARIZABLE)
// ============================================================================

int raft_datastore_set(raft_datastore_t *store,
                        const char *key,
                        const uint8_t *value,
                        size_t value_len,
                        int timeout_ms) {
    if (!store || !key || !value || value_len == 0) {
        return RESULT_ERR_INVALID;
    }
    
    if (value_len > RAFT_KV_MAX_VALUE_SIZE) {
        LOG_ERROR("Raft KV: Value too large (%zu > %d)", value_len, RAFT_KV_MAX_VALUE_SIZE);
        return RESULT_ERR_INVALID;
    }
    
    LOG_DEBUG("Raft KV: SET request for key=%s, len=%zu", key, value_len);
    
    // 1. Serialize command
    uint8_t cmd_buffer[RAFT_KV_MAX_VALUE_SIZE + 512];
    size_t cmd_len = raft_cmd_serialize_set(key, value, value_len,
                                             cmd_buffer, sizeof(cmd_buffer));
    
    if (cmd_len == 0) {
        LOG_ERROR("Raft KV: Failed to serialize SET command");
        return RESULT_ERR_INVALID;
    }
    
    // 2. Submit to Raft
    uint64_t log_index, log_term;
    int result = raft_submit_command(store->raft_state, cmd_buffer, cmd_len,
                                     &log_index, &log_term);
    
    if (result != 0) {
        node_id_t leader = raft_get_leader(store->raft_state);
        LOG_DEBUG("Raft KV: Not leader (current leader: %u)", leader);
        return RESULT_ERR_INVALID;
    }
    
    LOG_DEBUG("Raft KV: Command submitted (index=%lu, term=%lu)", log_index, log_term);
    
    // 3. Wait for commit
    result = raft_wait_committed(store->raft_state, log_index, timeout_ms);
    
    if (result != 0) {
        LOG_ERROR("Raft KV: Timeout waiting for commit");
        return RESULT_ERR_TIMEOUT;
    }
    
    LOG_INFO("Raft KV: SET committed - key=%s, index=%lu", key, log_index);
    
    return RESULT_OK;
}

int raft_datastore_get(raft_datastore_t *store,
                        const char *key,
                        uint8_t **out_value,
                        size_t *out_len,
                        int timeout_ms) {
    if (!store || !key || !out_value || !out_len) {
        return RESULT_ERR_INVALID;
    }
    
    *out_value = NULL;
    *out_len = 0;
    
    // For linearizable reads, we need to ensure we're reading from leader
    // and that our state is up-to-date
    if (!raft_is_leader(store->raft_state)) {
        LOG_DEBUG("Raft KV: Cannot serve read, not leader");
        return RESULT_ERR_INVALID;
    }
    
    store->total_gets++;
    
    pthread_rwlock_rdlock(&store->lock);
    
    raft_kv_record_t *record = find_record(store, key);
    
    if (!record) {
        pthread_rwlock_unlock(&store->lock);
        LOG_DEBUG("Raft KV: GET %s - not found", key);
        return RESULT_ERR_NOTFOUND;
    }
    
    // Deep copy value
    *out_value = safe_malloc(record->value_len);
    if (!*out_value) {
        pthread_rwlock_unlock(&store->lock);
        return RESULT_ERR_NOMEM;
    }
    
    memcpy(*out_value, record->value, record->value_len);
    *out_len = record->value_len;
    
    pthread_rwlock_unlock(&store->lock);
    
    LOG_DEBUG("Raft KV: GET %s - found (len=%zu)", key, *out_len);
    
    return RESULT_OK;
}

int raft_datastore_unset(raft_datastore_t *store,
                          const char *key,
                          int timeout_ms) {
    if (!store || !key) {
        return RESULT_ERR_INVALID;
    }
    
    LOG_DEBUG("Raft KV: UNSET request for key=%s", key);
    
    // 1. Serialize command
    uint8_t cmd_buffer[512];
    size_t cmd_len = raft_cmd_serialize_unset(key, cmd_buffer, sizeof(cmd_buffer));
    
    if (cmd_len == 0) {
        LOG_ERROR("Raft KV: Failed to serialize UNSET command");
        return RESULT_ERR_INVALID;
    }
    
    // 2. Submit to Raft
    uint64_t log_index, log_term;
    int result = raft_submit_command(store->raft_state, cmd_buffer, cmd_len,
                                     &log_index, &log_term);
    
    if (result != 0) {
        LOG_DEBUG("Raft KV: Not leader, cannot UNSET");
        return RESULT_ERR_INVALID;
    }
    
    // 3. Wait for commit
    result = raft_wait_committed(store->raft_state, log_index, timeout_ms);
    
    if (result != 0) {
        LOG_ERROR("Raft KV: Timeout waiting for commit");
        return RESULT_ERR_TIMEOUT;
    }
    
    LOG_INFO("Raft KV: UNSET committed - key=%s, index=%lu", key, log_index);
    
    return RESULT_OK;
}

size_t raft_datastore_list_keys(raft_datastore_t *store,
                                 char **out_keys,
                                 size_t max_count) {
    if (!store || !out_keys || max_count == 0) {
        return 0;
    }
    
    pthread_rwlock_rdlock(&store->lock);
    
    size_t found = 0;
    for (size_t i = 0; i < store->capacity && found < max_count; i++) {
        if (store->records[i].active) {
            out_keys[found++] = store->records[i].key;
        }
    }
    
    pthread_rwlock_unlock(&store->lock);
    
    return found;
}

// ============================================================================
// STATISTICS
// ============================================================================

void raft_datastore_get_stats(raft_datastore_t *store,
                               raft_datastore_stats_t *out_stats) {
    if (!store || !out_stats) {
        return;
    }
    
    memset(out_stats, 0, sizeof(raft_datastore_stats_t));
    
    pthread_rwlock_rdlock(&store->lock);
    
    out_stats->record_count = store->count;
    out_stats->capacity = store->capacity;
    out_stats->total_sets = store->total_sets;
    out_stats->total_gets = store->total_gets;
    out_stats->total_unsets = store->total_unsets;
    
    // Calculate total bytes
    size_t total_bytes = 0;
    for (size_t i = 0; i < store->capacity; i++) {
        if (store->records[i].active) {
            total_bytes += store->records[i].value_len;
        }
    }
    out_stats->total_bytes = total_bytes;
    
    pthread_rwlock_unlock(&store->lock);
}
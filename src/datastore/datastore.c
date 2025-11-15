// src/datastore/datastore.c
// Generic distributed key-value datastore implementation

#define _POSIX_C_SOURCE 200809L

#include "roole/datastore/datastore.h"
#include "roole/core/common.h"
#include "roole/logger/logger.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <arpa/inet.h>
#include <endian.h>

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

static datastore_record_t* find_record_unsafe(datastore_t *store, const char *key) {
    for (size_t i = 0; i < store->capacity; i++) {
        if (store->records[i].active && !store->records[i].tombstone &&
            strcmp(store->records[i].key, key) == 0) {
            return &store->records[i];
        }
    }
    return NULL;
}

static datastore_record_t* find_free_slot_unsafe(datastore_t *store) {
    for (size_t i = 0; i < store->capacity; i++) {
        if (!store->records[i].active) {
            return &store->records[i];
        }
    }
    return NULL;
}

static void notify_change(datastore_t *store, const char *key, const char *operation) {
    if (store->change_callback) {
        store->change_callback(key, operation, store->callback_user_data);
    }
}

// ============================================================================
// KEY VALIDATION
// ============================================================================

int datastore_validate_key(const char *key) {
    if (!key || key[0] == '\0') {
        return 0;  // Empty key
    }
    
    size_t len = strlen(key);
    if (len >= MAX_KEY_LEN) {
        return 0;  // Too long
    }
    
    // Allow: alphanumeric, underscore, hyphen, dot, colon, slash
    for (size_t i = 0; i < len; i++) {
        char c = key[i];
        if (!isalnum(c) && c != '_' && c != '-' && c != '.' && 
            c != ':' && c != '/') {
            return 0;  // Invalid character
        }
    }
    
    return 1;
}

// ============================================================================
// LIFECYCLE
// ============================================================================

int datastore_init(datastore_t *store, size_t capacity) {
    if (!store || capacity == 0) {
        LOG_ERROR("Invalid datastore init parameters");
        return RESULT_ERR_INVALID;
    }
    
    memset(store, 0, sizeof(datastore_t));
    
    store->records = (datastore_record_t*)safe_calloc(capacity, 
                                                       sizeof(datastore_record_t));
    if (!store->records) {
        LOG_ERROR("Failed to allocate datastore records");
        return RESULT_ERR_NOMEM;
    }
    
    store->capacity = capacity;
    store->count = 0;
    
    if (pthread_rwlock_init(&store->lock, NULL) != 0) {
        LOG_ERROR("Failed to initialize datastore rwlock");
        safe_free(store->records);
        return RESULT_ERR_INVALID;
    }
    
    LOG_INFO("Datastore initialized (capacity: %zu records, max_value: %d bytes)",
             capacity, MAX_VALUE_SIZE);
    
    return RESULT_OK;
}

void datastore_destroy(datastore_t *store) {
    if (!store) return;
    
    pthread_rwlock_wrlock(&store->lock);
    
    // Free all record values
    for (size_t i = 0; i < store->capacity; i++) {
        if (store->records[i].active && store->records[i].value) {
            safe_free(store->records[i].value);
        }
    }
    
    safe_free(store->records);
    store->records = NULL;
    store->capacity = 0;
    store->count = 0;
    
    pthread_rwlock_unlock(&store->lock);
    pthread_rwlock_destroy(&store->lock);
    
    LOG_INFO("Datastore destroyed");
}

// ============================================================================
// CRUD OPERATIONS
// ============================================================================

int datastore_set(datastore_t *store, const char *key,
                  const uint8_t *value, size_t value_len,
                  node_id_t owner_node) {
    if (!store || !key || !value || value_len == 0) {
        LOG_ERROR("Invalid datastore_set parameters");
        return RESULT_ERR_INVALID;
    }
    
    if (!datastore_validate_key(key)) {
        LOG_ERROR("Invalid key format: %s", key);
        return RESULT_ERR_INVALID;
    }
    
    if (value_len > MAX_VALUE_SIZE) {
        LOG_ERROR("Value too large: %zu > %d", value_len, MAX_VALUE_SIZE);
        return RESULT_ERR_INVALID;
    }
    
    pthread_rwlock_wrlock(&store->lock);
    
    // Check if key already exists
    datastore_record_t *existing = find_record_unsafe(store, key);
    
    if (existing) {
        // Update existing record
        LOG_DEBUG("Updating existing record: %s (old_len=%zu, new_len=%zu)",
                 key, existing->value_len, value_len);
        
        // Free old value
        if (existing->value) {
            safe_free(existing->value);
        }
        
        // Allocate new value
        existing->value = (uint8_t*)safe_malloc(value_len);
        if (!existing->value) {
            pthread_rwlock_unlock(&store->lock);
            LOG_ERROR("Failed to allocate value for key: %s", key);
            return RESULT_ERR_NOMEM;
        }
        
        memcpy(existing->value, value, value_len);
        existing->value_len = value_len;
        existing->version++;
        existing->updated_at_ms = time_now_ms();
        existing->owner_node = owner_node;
        existing->tombstone = 0;
        
        store->total_sets++;
        
        pthread_rwlock_unlock(&store->lock);
        
        notify_change(store, key, "set");
        
        LOG_INFO("Record updated: key=%s, version=%lu, len=%zu",
                key, existing->version, value_len);
        
        return RESULT_OK;
    }
    
    // Insert new record
    datastore_record_t *slot = find_free_slot_unsafe(store);
    if (!slot) {
        pthread_rwlock_unlock(&store->lock);
        LOG_ERROR("Datastore full (capacity: %zu)", store->capacity);
        return RESULT_ERR_FULL;
    }
    
    // Initialize new record
    memset(slot, 0, sizeof(datastore_record_t));
    safe_strncpy(slot->key, key, MAX_KEY_LEN);
    
    slot->value = (uint8_t*)safe_malloc(value_len);
    if (!slot->value) {
        pthread_rwlock_unlock(&store->lock);
        LOG_ERROR("Failed to allocate value for key: %s", key);
        return RESULT_ERR_NOMEM;
    }
    
    memcpy(slot->value, value, value_len);
    slot->value_len = value_len;
    slot->version = 1;
    slot->created_at_ms = time_now_ms();
    slot->updated_at_ms = slot->created_at_ms;
    slot->owner_node = owner_node;
    slot->active = 1;
    slot->tombstone = 0;
    
    store->count++;
    store->total_sets++;
    
    pthread_rwlock_unlock(&store->lock);
    
    notify_change(store, key, "set");
    
    LOG_INFO("Record created: key=%s, version=%lu, len=%zu, owner=%u",
            key, slot->version, value_len, owner_node);
    
    return RESULT_OK;
}

datastore_record_t* datastore_get(datastore_t *store, const char *key) {
    if (!store || !key) {
        return NULL;
    }
    
    pthread_rwlock_rdlock(&store->lock);
    
    datastore_record_t *record = find_record_unsafe(store, key);
    
    if (record) {
        store->total_gets++;
        LOG_DEBUG("Record retrieved: key=%s, version=%lu, len=%zu",
                 key, record->version, record->value_len);
    } else {
        pthread_rwlock_unlock(&store->lock);
        LOG_DEBUG("Record not found: key=%s", key);
        return NULL;
    }
    
    // Lock is held - caller MUST call datastore_release()
    return record;
}

void datastore_release(datastore_t *store) {
    if (store) {
        pthread_rwlock_unlock(&store->lock);
    }
}

int datastore_unset(datastore_t *store, const char *key) {
    if (!store || !key) {
        return RESULT_ERR_INVALID;
    }
    
    pthread_rwlock_wrlock(&store->lock);
    
    datastore_record_t *record = find_record_unsafe(store, key);
    if (!record) {
        pthread_rwlock_unlock(&store->lock);
        LOG_DEBUG("Record not found for unset: %s", key);
        return RESULT_ERR_NOTFOUND;
    }
    
    // Free value
    if (record->value) {
        safe_free(record->value);
        record->value = NULL;
    }
    
    // Mark as tombstone for gossip (will be cleaned up later)
    record->tombstone = 1;
    record->updated_at_ms = time_now_ms();
    record->version++;
    
    store->count--;
    store->total_unsets++;
    
    pthread_rwlock_unlock(&store->lock);
    
    notify_change(store, key, "unset");
    
    LOG_INFO("Record unset: key=%s (tombstone)", key);
    
    return RESULT_OK;
}

size_t datastore_list_keys(datastore_t *store, char **out_keys, size_t max_count) {
    if (!store || !out_keys || max_count == 0) {
        return 0;
    }
    
    pthread_rwlock_rdlock(&store->lock);
    
    size_t found = 0;
    for (size_t i = 0; i < store->capacity && found < max_count; i++) {
        if (store->records[i].active && !store->records[i].tombstone) {
            out_keys[found++] = store->records[i].key;
        }
    }
    
    pthread_rwlock_unlock(&store->lock);
    
    return found;
}

size_t datastore_count(datastore_t *store) {
    if (!store) return 0;
    
    pthread_rwlock_rdlock(&store->lock);
    size_t count = store->count;
    pthread_rwlock_unlock(&store->lock);
    
    return count;
}

// ============================================================================
// DISTRIBUTED OPERATIONS
// ============================================================================

int datastore_merge_record(datastore_t *store, const datastore_record_t *record) {
    if (!store || !record) {
        return RESULT_ERR_INVALID;
    }
    
    pthread_rwlock_wrlock(&store->lock);
    
    datastore_record_t *local = find_record_unsafe(store, record->key);
    
    if (!local) {
        // New record - insert it
        datastore_record_t *slot = find_free_slot_unsafe(store);
        if (!slot) {
            pthread_rwlock_unlock(&store->lock);
            LOG_ERROR("Datastore full, cannot merge record: %s", record->key);
            return RESULT_ERR_FULL;
        }
        
        // Deep copy
        datastore_copy_record(slot, record);
        slot->active = 1;
        store->count++;
        
        pthread_rwlock_unlock(&store->lock);
        
        notify_change(store, record->key, "merge");
        
        LOG_INFO("Merged new record: key=%s, version=%lu, owner=%u",
                record->key, record->version, record->owner_node);
        
        return RESULT_OK;
    }
    
    // Conflict resolution: higher version wins
    if (record->version > local->version) {
        LOG_DEBUG("Merging remote record (newer version): key=%s, remote_v=%lu, local_v=%lu",
                 record->key, record->version, local->version);
        
        // Free old value
        if (local->value) {
            safe_free(local->value);
        }
        
        // Copy remote record
        datastore_copy_record(local, record);
        
        store->total_conflicts++;
        
        pthread_rwlock_unlock(&store->lock);
        
        notify_change(store, record->key, "merge");
        
        LOG_INFO("Record merged (version conflict resolved): key=%s, version=%lu",
                record->key, record->version);
        
        return RESULT_OK;
        
    } else if (record->version == local->version) {
        // Tie-breaker: higher owner_node wins
        if (record->owner_node > local->owner_node) {
            LOG_DEBUG("Merging remote record (tie-breaker): key=%s, remote_owner=%u, local_owner=%u",
                     record->key, record->owner_node, local->owner_node);
            
            if (local->value) {
                safe_free(local->value);
            }
            
            datastore_copy_record(local, record);
            store->total_conflicts++;
            
            pthread_rwlock_unlock(&store->lock);
            notify_change(store, record->key, "merge");
            
            return RESULT_OK;
        }
    }
    
    // Local version is newer or equal with lower node ID
    pthread_rwlock_unlock(&store->lock);
    
    LOG_DEBUG("Local record is newer, ignoring merge: key=%s", record->key);
    
    return RESULT_ERR_EXISTS;
}

int datastore_get_all_records(datastore_t *store,
                              datastore_record_t **out_records,
                              size_t *out_count) {
    if (!store || !out_records || !out_count) {
        return RESULT_ERR_INVALID;
    }
    
    pthread_rwlock_rdlock(&store->lock);
    
    size_t count = store->count;
    
    if (count == 0) {
        *out_records = NULL;
        *out_count = 0;
        pthread_rwlock_unlock(&store->lock);
        return RESULT_OK;
    }
    
    datastore_record_t *records = (datastore_record_t*)safe_malloc(
        count * sizeof(datastore_record_t));
    
    if (!records) {
        pthread_rwlock_unlock(&store->lock);
        return RESULT_ERR_NOMEM;
    }
    
    size_t idx = 0;
    for (size_t i = 0; i < store->capacity && idx < count; i++) {
        if (store->records[i].active && !store->records[i].tombstone) {
            datastore_copy_record(&records[idx++], &store->records[i]);
        }
    }
    
    pthread_rwlock_unlock(&store->lock);
    
    *out_records = records;
    *out_count = idx;
    
    LOG_DEBUG("Retrieved all records: count=%zu", idx);
    
    return RESULT_OK;
}

int datastore_get_modified_since(datastore_t *store, uint64_t since_ms,
                                 datastore_record_t **out_records,
                                 size_t *out_count) {
    if (!store || !out_records || !out_count) {
        return RESULT_ERR_INVALID;
    }
    
    pthread_rwlock_rdlock(&store->lock);
    
    // Count modified records
    size_t count = 0;
    for (size_t i = 0; i < store->capacity; i++) {
        if (store->records[i].active && 
            store->records[i].updated_at_ms > since_ms) {
            count++;
        }
    }
    
    if (count == 0) {
        *out_records = NULL;
        *out_count = 0;
        pthread_rwlock_unlock(&store->lock);
        return RESULT_OK;
    }
    
    datastore_record_t *records = (datastore_record_t*)safe_malloc(
        count * sizeof(datastore_record_t));
    
    if (!records) {
        pthread_rwlock_unlock(&store->lock);
        return RESULT_ERR_NOMEM;
    }
    
    size_t idx = 0;
    for (size_t i = 0; i < store->capacity && idx < count; i++) {
        if (store->records[i].active && 
            store->records[i].updated_at_ms > since_ms) {
            datastore_copy_record(&records[idx++], &store->records[i]);
        }
    }
    
    pthread_rwlock_unlock(&store->lock);
    
    *out_records = records;
    *out_count = idx;
    
    LOG_DEBUG("Retrieved modified records: count=%zu, since=%lu", idx, since_ms);
    
    return RESULT_OK;
}

// ============================================================================
// RECORD UTILITIES
// ============================================================================

int datastore_copy_record(datastore_record_t *dest, const datastore_record_t *src) {
    if (!dest || !src) {
        return RESULT_ERR_INVALID;
    }
    
    memcpy(dest, src, sizeof(datastore_record_t));
    
    // Deep copy value
    if (src->value && src->value_len > 0) {
        dest->value = (uint8_t*)safe_malloc(src->value_len);
        if (!dest->value) {
            return RESULT_ERR_NOMEM;
        }
        memcpy(dest->value, src->value, src->value_len);
    } else {
        dest->value = NULL;
    }
    
    return RESULT_OK;
}

void datastore_free_record(datastore_record_t *record) {
    if (!record) return;
    
    if (record->value) {
        safe_free(record->value);
        record->value = NULL;
    }
    record->value_len = 0;
}

// ============================================================================
// CALLBACKS AND OBSERVABILITY
// ============================================================================

void datastore_set_change_callback(datastore_t *store,
                                   void (*callback)(const char *, const char *, void *),
                                   void *user_data) {
    if (!store) return;
    
    pthread_rwlock_wrlock(&store->lock);
    store->change_callback = callback;
    store->callback_user_data = user_data;
    pthread_rwlock_unlock(&store->lock);
}

void datastore_get_stats(datastore_t *store, datastore_stats_t *out_stats) {
    if (!store || !out_stats) return;
    
    memset(out_stats, 0, sizeof(datastore_stats_t));
    
    pthread_rwlock_rdlock(&store->lock);
    
    out_stats->active_records = store->count;
    out_stats->capacity = store->capacity;
    out_stats->total_sets = store->total_sets;
    out_stats->total_gets = store->total_gets;
    out_stats->total_unsets = store->total_unsets;
    out_stats->total_conflicts = store->total_conflicts;
    
    // Calculate total value bytes
    size_t total_bytes = 0;
    for (size_t i = 0; i < store->capacity; i++) {
        if (store->records[i].active && !store->records[i].tombstone) {
            total_bytes += store->records[i].value_len;
        }
    }
    out_stats->total_value_bytes = total_bytes;
    
    pthread_rwlock_unlock(&store->lock);
}

void datastore_dump(datastore_t *store, const char *label) {
    if (!store) return;
    
    pthread_rwlock_rdlock(&store->lock);
    
    LOG_INFO("========================================");
    LOG_INFO("Datastore Dump: %s", label ? label : "");
    LOG_INFO("  Capacity: %zu", store->capacity);
    LOG_INFO("  Active Records: %zu", store->count);
    LOG_INFO("  Total Sets: %lu", store->total_sets);
    LOG_INFO("  Total Gets: %lu", store->total_gets);
    LOG_INFO("  Total Unsets: %lu", store->total_unsets);
    LOG_INFO("  Conflicts Resolved: %lu", store->total_conflicts);
    LOG_INFO("========================================");
    
    size_t shown = 0;
    for (size_t i = 0; i < store->capacity && shown < 10; i++) {
        if (store->records[i].active && !store->records[i].tombstone) {
            LOG_INFO("  [%zu] key=%s, version=%lu, len=%zu, owner=%u",
                    shown++,
                    store->records[i].key,
                    store->records[i].version,
                    store->records[i].value_len,
                    store->records[i].owner_node);
        }
    }
    
    if (store->count > 10) {
        LOG_INFO("  ... (%zu more records)", store->count - 10);
    }
    
    LOG_INFO("========================================");
    
    pthread_rwlock_unlock(&store->lock);
}
// src/datastore/datastore_serialization.c
// Serialization for network transfer and gossip

#define _POSIX_C_SOURCE 200809L

#include "roole/datastore/datastore.h"
#include "roole/core/common.h"
#include "roole/logger/logger.h"
#include <string.h>
#include <arpa/inet.h>
#include <endian.h>

// ============================================================================
// SINGLE RECORD SERIALIZATION
// ============================================================================

size_t datastore_serialize_record(const datastore_record_t *record,
                                  uint8_t *buffer, size_t buffer_size) {
    if (!record || !buffer || buffer_size == 0) {
        LOG_ERROR("Invalid serialize parameters");
        return 0;
    }
    
    // Calculate required size
    uint16_t key_len = (uint16_t)strlen(record->key);
    size_t required_size = 2 + key_len +  // key_len + key
                          4 +              // value_len
                          record->value_len +
                          8 +              // version
                          8 +              // created_at
                          8 +              // updated_at
                          2 +              // owner_node
                          1;               // tombstone
    
    if (buffer_size < required_size) {
        LOG_ERROR("Buffer too small for serialization: need %zu, have %zu",
                 required_size, buffer_size);
        return 0;
    }
    
    uint8_t *ptr = buffer;
    
    // Key length and key
    uint16_t key_len_net = htons(key_len);
    memcpy(ptr, &key_len_net, 2);
    ptr += 2;
    
    memcpy(ptr, record->key, key_len);
    ptr += key_len;
    
    // Value length and value
    uint32_t value_len_net = htonl((uint32_t)record->value_len);
    memcpy(ptr, &value_len_net, 4);
    ptr += 4;
    
    if (record->value && record->value_len > 0) {
        memcpy(ptr, record->value, record->value_len);
        ptr += record->value_len;
    }
    
    // Version
    uint64_t version_net = htobe64(record->version);
    memcpy(ptr, &version_net, 8);
    ptr += 8;
    
    // Timestamps
    uint64_t created_net = htobe64(record->created_at_ms);
    memcpy(ptr, &created_net, 8);
    ptr += 8;
    
    uint64_t updated_net = htobe64(record->updated_at_ms);
    memcpy(ptr, &updated_net, 8);
    ptr += 8;
    
    // Owner node
    uint16_t owner_net = htons(record->owner_node);
    memcpy(ptr, &owner_net, 2);
    ptr += 2;
    
    // Tombstone flag
    *ptr++ = record->tombstone ? 1 : 0;
    
    size_t serialized_size = ptr - buffer;
    
    LOG_DEBUG("Serialized record: key=%s, size=%zu bytes",
             record->key, serialized_size);
    
    return serialized_size;
}

int datastore_deserialize_record(const uint8_t *buffer, size_t buffer_len,
                                 datastore_record_t *out_record) {
    if (!buffer || !out_record || buffer_len < 2) {
        LOG_ERROR("Invalid deserialize parameters");
        return RESULT_ERR_INVALID;
    }
    
    memset(out_record, 0, sizeof(datastore_record_t));
    
    const uint8_t *ptr = buffer;
    const uint8_t *end = buffer + buffer_len;
    
    // Key length and key
    if (ptr + 2 > end) return RESULT_ERR_INVALID;
    uint16_t key_len_net;
    memcpy(&key_len_net, ptr, 2);
    uint16_t key_len = ntohs(key_len_net);
    ptr += 2;
    
    if (key_len >= MAX_KEY_LEN || ptr + key_len > end) {
        LOG_ERROR("Invalid key length: %u", key_len);
        return RESULT_ERR_INVALID;
    }
    
    memcpy(out_record->key, ptr, key_len);
    out_record->key[key_len] = '\0';
    ptr += key_len;
    
    // Value length and value
    if (ptr + 4 > end) return RESULT_ERR_INVALID;
    uint32_t value_len_net;
    memcpy(&value_len_net, ptr, 4);
    uint32_t value_len = ntohl(value_len_net);
    ptr += 4;
    
    if (value_len > MAX_VALUE_SIZE) {
        LOG_ERROR("Value too large: %u > %d", value_len, MAX_VALUE_SIZE);
        return RESULT_ERR_INVALID;
    }
    
    if (value_len > 0) {
        if (ptr + value_len > end) return RESULT_ERR_INVALID;
        
        out_record->value = (uint8_t*)safe_malloc(value_len);
        if (!out_record->value) {
            return RESULT_ERR_NOMEM;
        }
        
        memcpy(out_record->value, ptr, value_len);
        out_record->value_len = value_len;
        ptr += value_len;
    } else {
        out_record->value = NULL;
        out_record->value_len = 0;
    }
    
    // Version
    if (ptr + 8 > end) {
        if (out_record->value) safe_free(out_record->value);
        return RESULT_ERR_INVALID;
    }
    uint64_t version_net;
    memcpy(&version_net, ptr, 8);
    out_record->version = be64toh(version_net);
    ptr += 8;
    
    // Timestamps
    if (ptr + 8 > end) {
        if (out_record->value) safe_free(out_record->value);
        return RESULT_ERR_INVALID;
    }
    uint64_t created_net;
    memcpy(&created_net, ptr, 8);
    out_record->created_at_ms = be64toh(created_net);
    ptr += 8;
    
    if (ptr + 8 > end) {
        if (out_record->value) safe_free(out_record->value);
        return RESULT_ERR_INVALID;
    }
    uint64_t updated_net;
    memcpy(&updated_net, ptr, 8);
    out_record->updated_at_ms = be64toh(updated_net);
    ptr += 8;
    
    // Owner node
    if (ptr + 2 > end) {
        if (out_record->value) safe_free(out_record->value);
        return RESULT_ERR_INVALID;
    }
    uint16_t owner_net;
    memcpy(&owner_net, ptr, 2);
    out_record->owner_node = ntohs(owner_net);
    ptr += 2;
    
    // Tombstone
    if (ptr + 1 > end) {
        if (out_record->value) safe_free(out_record->value);
        return RESULT_ERR_INVALID;
    }
    out_record->tombstone = (*ptr++ == 1) ? 1 : 0;
    
    out_record->active = 1;
    
    LOG_DEBUG("Deserialized record: key=%s, version=%lu, len=%zu",
             out_record->key, out_record->version, out_record->value_len);
    
    return RESULT_OK;
}

// ============================================================================
// MULTIPLE RECORDS SERIALIZATION
// ============================================================================

size_t datastore_serialize_records(const datastore_record_t *records, size_t count,
                                   uint8_t *buffer, size_t buffer_size) {
    if (!records || count == 0 || !buffer || buffer_size < 4) {
        return 0;
    }
    
    uint8_t *ptr = buffer;
    const uint8_t *end = buffer + buffer_size;
    
    // Write count
    uint32_t count_net = htonl((uint32_t)count);
    memcpy(ptr, &count_net, 4);
    ptr += 4;
    
    // Serialize each record
    for (size_t i = 0; i < count; i++) {
        size_t remaining = end - ptr;
        size_t serialized = datastore_serialize_record(&records[i], ptr, remaining);
        
        if (serialized == 0) {
            LOG_ERROR("Failed to serialize record %zu/%zu", i + 1, count);
            return 0;
        }
        
        ptr += serialized;
    }
    
    size_t total_size = ptr - buffer;
    
    LOG_INFO("Serialized %zu records: %zu bytes total", count, total_size);
    
    return total_size;
}

int datastore_deserialize_records(const uint8_t *buffer, size_t buffer_len,
                                  datastore_record_t **out_records,
                                  size_t *out_count) {
    if (!buffer || !out_records || !out_count || buffer_len < 4) {
        return RESULT_ERR_INVALID;
    }
    
    const uint8_t *ptr = buffer;
    const uint8_t *end = buffer + buffer_len;
    
    // Read count
    uint32_t count_net;
    memcpy(&count_net, ptr, 4);
    uint32_t count = ntohl(count_net);
    ptr += 4;
    
    if (count == 0) {
        *out_records = NULL;
        *out_count = 0;
        return RESULT_OK;
    }
    
    if (count > MAX_RECORDS) {
        LOG_ERROR("Too many records: %u > %d", count, MAX_RECORDS);
        return RESULT_ERR_INVALID;
    }
    
    // Allocate output array
    datastore_record_t *records = (datastore_record_t*)safe_calloc(
        count, sizeof(datastore_record_t));
    
    if (!records) {
        return RESULT_ERR_NOMEM;
    }
    
    // Deserialize each record
    for (size_t i = 0; i < count; i++) {
        size_t remaining = end - ptr;
        int result = datastore_deserialize_record(ptr, remaining, &records[i]);
        
        if (result != RESULT_OK) {
            LOG_ERROR("Failed to deserialize record %zu/%u", i + 1, count);
            
            // Cleanup already deserialized records
            for (size_t j = 0; j < i; j++) {
                datastore_free_record(&records[j]);
            }
            safe_free(records);
            
            return result;
        }
        
        // Move pointer forward by serialized size
        // We need to recalculate size since deserialize doesn't return it
        uint16_t key_len = strlen(records[i].key);
        size_t record_size = 2 + key_len + 4 + records[i].value_len + 
                            8 + 8 + 8 + 2 + 1;
        ptr += record_size;
    }
    
    *out_records = records;
    *out_count = count;
    
    LOG_INFO("Deserialized %u records successfully", count);
    
    return RESULT_OK;
}
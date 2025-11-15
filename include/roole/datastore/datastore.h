#ifndef ROOLE_DATASTORE_H
#define ROOLE_DATASTORE_H

#include "roole/core/common.h"
#include <pthread.h>
#include <stdint.h>
#include <stddef.h>

// ============================================================================
// CONFIGURATION
// ============================================================================

#define MAX_KEY_LEN 256
#define MAX_VALUE_SIZE (1024 * 1024)  // 1MB max per record
#define MAX_RECORDS 10000
#define DATASTORE_VERSION 1

// ============================================================================
// RECORD STRUCTURE
// ============================================================================

typedef struct datastore_record {
    char key[MAX_KEY_LEN];           // Unique key identifier
    uint8_t *value;                  // Opaque value data (allocated)
    size_t value_len;                // Length of value
    
    // Metadata for conflict resolution
    uint64_t version;                // Logical timestamp / version
    uint64_t created_at_ms;          // Creation timestamp
    uint64_t updated_at_ms;          // Last update timestamp
    node_id_t owner_node;            // Node that created the record
    
    // Internal flags
    int active;                      // 1 if slot is in use
    int tombstone;                   // 1 if deleted (for gossip)
} datastore_record_t;

// ============================================================================
// DATASTORE STRUCTURE
// ============================================================================

typedef struct datastore {
    datastore_record_t *records;     // Array of records
    size_t capacity;                 // Maximum records
    size_t count;                    // Current active records
    
    pthread_rwlock_t lock;           // Read-write lock
    
    // Change notification callback
    void (*change_callback)(const char *key, const char *operation, void *user_data);
    void *callback_user_data;
    
    // Statistics
    uint64_t total_sets;
    uint64_t total_gets;
    uint64_t total_unsets;
    uint64_t total_conflicts;
} datastore_t;

// ============================================================================
// DATASTORE LIFECYCLE
// ============================================================================

/**
 * Initialize datastore
 * @param store Datastore structure
 * @param capacity Maximum number of records
 * @return RESULT_OK on success, error code on failure
 */
int datastore_init(datastore_t *store, size_t capacity);

/**
 * Destroy datastore
 * Frees all records and allocated memory
 * @param store Datastore structure
 */
void datastore_destroy(datastore_t *store);

// ============================================================================
// CRUD OPERATIONS
// ============================================================================

/**
 * Set record (insert or update)
 * Thread-safe: acquires write lock
 * @param store Datastore structure
 * @param key Record key (null-terminated string)
 * @param value Value data
 * @param value_len Length of value
 * @param owner_node Node ID that owns this record
 * @return RESULT_OK on success, error code on failure
 */
int datastore_set(datastore_t *store, const char *key, 
                  const uint8_t *value, size_t value_len,
                  node_id_t owner_node);

/**
 * Get record
 * Thread-safe: acquires read lock
 * Caller must call datastore_release() when done with record
 * @param store Datastore structure
 * @param key Record key
 * @return Pointer to record, or NULL if not found
 */
datastore_record_t* datastore_get(datastore_t *store, const char *key);

/**
 * Release read lock after datastore_get()
 * @param store Datastore structure
 */
void datastore_release(datastore_t *store);

/**
 * Unset (delete) record
 * Thread-safe: acquires write lock
 * @param store Datastore structure
 * @param key Record key
 * @return RESULT_OK on success, RESULT_ERR_NOTFOUND if key doesn't exist
 */
int datastore_unset(datastore_t *store, const char *key);

/**
 * List all keys
 * Thread-safe: acquires read lock
 * @param store Datastore structure
 * @param out_keys Output array of key pointers (allocated by caller)
 * @param max_count Maximum number of keys to return
 * @return Number of keys returned
 */
size_t datastore_list_keys(datastore_t *store, char **out_keys, size_t max_count);

/**
 * Get number of active records
 * Thread-safe: acquires read lock
 * @param store Datastore structure
 * @return Number of active records
 */
size_t datastore_count(datastore_t *store);

// ============================================================================
// DISTRIBUTED OPERATIONS
// ============================================================================

/**
 * Merge remote record (for gossip/replication)
 * Applies conflict resolution:
 * - Higher version wins
 * - If versions equal, higher owner_node wins (tie-breaker)
 * Thread-safe: acquires write lock
 * @param store Datastore structure
 * @param record Remote record to merge
 * @return RESULT_OK if merged, RESULT_ERR_EXISTS if local version is newer
 */
int datastore_merge_record(datastore_t *store, const datastore_record_t *record);

/**
 * Get all records (for full sync)
 * Thread-safe: acquires read lock
 * Caller must free returned array and call datastore_release()
 * @param store Datastore structure
 * @param out_records Output array pointer (allocated by function)
 * @param out_count Output count
 * @return RESULT_OK on success
 */
int datastore_get_all_records(datastore_t *store, 
                              datastore_record_t **out_records,
                              size_t *out_count);

/**
 * Get records modified after timestamp
 * Useful for delta sync
 * @param store Datastore structure
 * @param since_ms Timestamp threshold
 * @param out_records Output array pointer (allocated by function)
 * @param out_count Output count
 * @return RESULT_OK on success
 */
int datastore_get_modified_since(datastore_t *store,
                                 uint64_t since_ms,
                                 datastore_record_t **out_records,
                                 size_t *out_count);

// ============================================================================
// SERIALIZATION (for network transfer)
// ============================================================================

/**
 * Serialize single record
 * Format: [key_len: 2][key][value_len: 4][value][version: 8]
 *         [created_at: 8][updated_at: 8][owner_node: 2][tombstone: 1]
 * @param record Record to serialize
 * @param buffer Output buffer
 * @param buffer_size Buffer size
 * @return Number of bytes written, or 0 on error
 */
size_t datastore_serialize_record(const datastore_record_t *record,
                                  uint8_t *buffer, size_t buffer_size);

/**
 * Deserialize single record
 * @param buffer Input buffer
 * @param buffer_len Buffer length
 * @param out_record Output record (memory allocated by caller)
 * @return RESULT_OK on success
 */
int datastore_deserialize_record(const uint8_t *buffer, size_t buffer_len,
                                 datastore_record_t *out_record);

/**
 * Serialize multiple records
 * Format: [count: 4][record1][record2]...
 * @param records Array of records
 * @param count Number of records
 * @param buffer Output buffer
 * @param buffer_size Buffer size
 * @return Number of bytes written, or 0 on error
 */
size_t datastore_serialize_records(const datastore_record_t *records, size_t count,
                                   uint8_t *buffer, size_t buffer_size);

/**
 * Deserialize multiple records
 * @param buffer Input buffer
 * @param buffer_len Buffer length
 * @param out_records Output array pointer (allocated by function)
 * @param out_count Output count
 * @return RESULT_OK on success
 */
int datastore_deserialize_records(const uint8_t *buffer, size_t buffer_len,
                                  datastore_record_t **out_records,
                                  size_t *out_count);

// ============================================================================
// CALLBACKS AND OBSERVABILITY
// ============================================================================

/**
 * Set change callback
 * Called when records are set/unset
 * @param store Datastore structure
 * @param callback Callback function (key, operation, user_data)
 *                 operation: "set", "unset", "merge"
 * @param user_data User data passed to callback
 */
void datastore_set_change_callback(datastore_t *store,
                                   void (*callback)(const char *key, const char *op, void *data),
                                   void *user_data);

/**
 * Get statistics
 * @param store Datastore structure
 * @param out_stats Output statistics structure
 */
typedef struct {
    size_t active_records;
    size_t capacity;
    uint64_t total_sets;
    uint64_t total_gets;
    uint64_t total_unsets;
    uint64_t total_conflicts;
    size_t total_value_bytes;
} datastore_stats_t;

void datastore_get_stats(datastore_t *store, datastore_stats_t *out_stats);

/**
 * Debug: dump datastore contents to log
 * @param store Datastore structure
 * @param label Label for dump
 */
void datastore_dump(datastore_t *store, const char *label);

// ============================================================================
// VALIDATION AND UTILITIES
// ============================================================================

/**
 * Validate key format
 * Keys must be:
 * - Non-empty
 * - <= MAX_KEY_LEN
 * - Alphanumeric + underscore, hyphen, dot, colon, slash
 * @param key Key to validate
 * @return 1 if valid, 0 if invalid
 */
int datastore_validate_key(const char *key);

/**
 * Copy record (deep copy)
 * Allocates new value buffer
 * @param dest Destination record
 * @param src Source record
 * @return RESULT_OK on success
 */
int datastore_copy_record(datastore_record_t *dest, const datastore_record_t *src);

/**
 * Free record value
 * @param record Record to free
 */
void datastore_free_record(datastore_record_t *record);

#endif // ROOLE_DATASTORE_H
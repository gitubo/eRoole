// include/roole/raft/raft_datastore.h
// Strongly consistent key-value store backed by Raft consensus

#ifndef ROOLE_RAFT_DATASTORE_H
#define ROOLE_RAFT_DATASTORE_H

#include "roole/raft/raft_state.h"
#include "roole/core/common.h"
#include <pthread.h>

// ============================================================================
// CONFIGURATION
// ============================================================================

#define RAFT_KV_MAX_KEY_LEN 256
#define RAFT_KV_MAX_VALUE_SIZE (1024 * 1024)  // 1MB
#define RAFT_KV_MAX_RECORDS 10000

// ============================================================================
// COMMAND TYPES
// ============================================================================

typedef enum {
    RAFT_CMD_SET = 1,    // Set key-value pair
    RAFT_CMD_UNSET = 2,  // Delete key
    RAFT_CMD_GET = 3,    // Read key (linearizable read)
} raft_command_type_t;

// ============================================================================
// KV RECORD
// ============================================================================

typedef struct raft_kv_record {
    char key[RAFT_KV_MAX_KEY_LEN];
    uint8_t *value;
    size_t value_len;
    uint64_t version;        // Monotonic version (log index)
    uint64_t created_at_ms;
    uint64_t updated_at_ms;
    int active;              // 1 if in use
} raft_kv_record_t;

// ============================================================================
// DATASTORE
// ============================================================================

typedef struct raft_datastore {
    // Raft state machine
    raft_state_t *raft_state;
    
    // Key-value storage
    raft_kv_record_t *records;
    size_t capacity;
    size_t count;
    pthread_rwlock_t lock;
    
    // Statistics
    uint64_t total_sets;
    uint64_t total_gets;
    uint64_t total_unsets;
    
    // Pending client requests (for linearizable reads)
    pthread_mutex_t pending_lock;
    pthread_cond_t commit_cond;
    
} raft_datastore_t;

// ============================================================================
// LIFECYCLE
// ============================================================================

/**
 * Create Raft-backed datastore
 * @param raft_state Raft consensus state machine
 * @param capacity Maximum number of records
 * @return Datastore handle, or NULL on error
 */
raft_datastore_t* raft_datastore_create(raft_state_t *raft_state, size_t capacity);

/**
 * Destroy datastore
 * @param store Datastore handle
 */
void raft_datastore_destroy(raft_datastore_t *store);

// ============================================================================
// OPERATIONS (Linearizable through Raft)
// ============================================================================

/**
 * Set key-value pair (strongly consistent write)
 * Submits command to Raft, waits for commit
 * @param store Datastore
 * @param key Key (null-terminated)
 * @param value Value data
 * @param value_len Value length
 * @param timeout_ms Timeout for commit
 * @return 0 on success, error code on failure
 */
int raft_datastore_set(raft_datastore_t *store,
                        const char *key,
                        const uint8_t *value,
                        size_t value_len,
                        int timeout_ms);

/**
 * Get value by key (linearizable read)
 * Implements read-index optimization (leader confirms it's still leader)
 * @param store Datastore
 * @param key Key (null-terminated)
 * @param out_value Output value (caller must free)
 * @param out_len Output length
 * @param timeout_ms Timeout for linearizability check
 * @return 0 on success, RESULT_ERR_NOTFOUND if not found
 */
int raft_datastore_get(raft_datastore_t *store,
                        const char *key,
                        uint8_t **out_value,
                        size_t *out_len,
                        int timeout_ms);

/**
 * Delete key (strongly consistent)
 * @param store Datastore
 * @param key Key (null-terminated)
 * @param timeout_ms Timeout for commit
 * @return 0 on success, error code on failure
 */
int raft_datastore_unset(raft_datastore_t *store,
                          const char *key,
                          int timeout_ms);

/**
 * List all keys (eventually consistent - reads local state)
 * @param store Datastore
 * @param out_keys Output array (allocated by caller)
 * @param max_count Maximum keys to return
 * @return Number of keys returned
 */
size_t raft_datastore_list_keys(raft_datastore_t *store,
                                 char **out_keys,
                                 size_t max_count);

// ============================================================================
// RAFT STATE MACHINE INTEGRATION
// ============================================================================

/**
 * Apply committed log entry to state machine
 * Called by Raft when entry is committed
 * @param entry Log entry
 * @param user_data Datastore pointer
 * @return 0 on success
 */
int raft_datastore_apply(const raft_log_entry_t *entry, void *user_data);

/**
 * Create snapshot of current state
 * Called by Raft when snapshot is needed
 * @param last_included_index Last index in snapshot
 * @param last_included_term Last term in snapshot
 * @param out_data Output snapshot (allocated)
 * @param out_len Output length
 * @param user_data Datastore pointer
 * @return 0 on success
 */
int raft_datastore_snapshot(uint64_t last_included_index,
                             uint64_t last_included_term,
                             uint8_t **out_data,
                             size_t *out_len,
                             void *user_data);

/**
 * Restore from snapshot
 * Called by Raft when installing snapshot
 * @param data Snapshot data
 * @param len Snapshot length
 * @param last_included_index Last index
 * @param last_included_term Last term
 * @param user_data Datastore pointer
 * @return 0 on success
 */
int raft_datastore_restore(const uint8_t *data,
                            size_t len,
                            uint64_t last_included_index,
                            uint64_t last_included_term,
                            void *user_data);

// ============================================================================
// COMMAND SERIALIZATION
// ============================================================================

/**
 * Serialize SET command
 * Format: [cmd_type:1][key_len:2][key][value_len:4][value]
 */
size_t raft_cmd_serialize_set(const char *key,
                               const uint8_t *value,
                               size_t value_len,
                               uint8_t *buffer,
                               size_t buffer_size);

/**
 * Serialize UNSET command
 * Format: [cmd_type:1][key_len:2][key]
 */
size_t raft_cmd_serialize_unset(const char *key,
                                 uint8_t *buffer,
                                 size_t buffer_size);

/**
 * Deserialize command and execute
 * @param data Command data
 * @param len Command length
 * @param store Datastore (for execution)
 * @return 0 on success
 */
int raft_cmd_deserialize_and_execute(const uint8_t *data,
                                      size_t len,
                                      raft_datastore_t *store);

// ============================================================================
// STATISTICS
// ============================================================================

typedef struct raft_datastore_stats {
    size_t record_count;
    size_t capacity;
    uint64_t total_sets;
    uint64_t total_gets;
    uint64_t total_unsets;
    size_t total_bytes;
} raft_datastore_stats_t;

void raft_datastore_get_stats(raft_datastore_t *store,
                               raft_datastore_stats_t *out_stats);

#endif // ROOLE_RAFT_DATASTORE_H
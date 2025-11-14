// include/roole/node/execution_tracker.h
// Execution state tracking

#ifndef ROOLE_NODE_EXECUTION_TRACKER_H
#define ROOLE_NODE_EXECUTION_TRACKER_H

#include "roole/core/common.h"
#include "roole/node/message_queue.h"
#include <pthread.h>

#define MAX_PENDING_EXECUTIONS 10000

// Execution status
typedef enum {
    EXEC_STATUS_PENDING = 0,
    EXEC_STATUS_RUNNING = 1,
    EXEC_STATUS_COMPLETED = 2,
    EXEC_STATUS_FAILED = 3,
    EXEC_STATUS_RETRYING = 4,
    EXEC_STATUS_TIMEOUT = 5
} execution_status_t;

// Execution record
typedef struct {
    execution_id_t exec_id;
    rule_id_t dag_id;
    node_id_t assigned_peer;
    execution_status_t status;
    
    uint64_t submit_time_ms;
    uint64_t start_time_ms;
    uint64_t complete_time_ms;
    
    uint8_t retry_count;
    uint8_t max_retries;
    
    uint8_t message_data[MAX_MESSAGE_SIZE];
    size_t message_len;
    
    int active;
} execution_record_t;

// Execution tracker
typedef struct {
    execution_record_t *records;
    size_t capacity;
    execution_id_t next_exec_id;
    pthread_rwlock_t lock;
} execution_tracker_t;

/**
 * Initialize execution tracker
 * @param tracker Tracker structure
 * @param capacity Maximum executions
 * @return 0 on success, error code on failure
 */
int execution_tracker_init(execution_tracker_t *tracker, size_t capacity);

/**
 * Destroy execution tracker
 * @param tracker Tracker structure
 */
void execution_tracker_destroy(execution_tracker_t *tracker);

/**
 * Add execution
 * @param tracker Tracker structure
 * @param dag_id DAG ID
 * @param peer_id Assigned peer ID
 * @param message Message data
 * @param message_len Message length
 * @param max_retries Maximum retries
 * @return Execution ID, or 0 on error
 */
execution_id_t execution_tracker_add(execution_tracker_t *tracker, rule_id_t dag_id,
                                     node_id_t peer_id, const uint8_t *message,
                                     size_t message_len, uint8_t max_retries);

/**
 * Update execution status
 * @param tracker Tracker structure
 * @param exec_id Execution ID
 * @param status New status
 * @return 0 on success, error code on failure
 */
int execution_tracker_update_status(execution_tracker_t *tracker, execution_id_t exec_id,
                                    execution_status_t status);

/**
 * Get execution record
 * Returns pointer (read lock held) - caller must call execution_tracker_release()
 * @param tracker Tracker structure
 * @param exec_id Execution ID
 * @return Execution record, or NULL if not found
 */
execution_record_t* execution_tracker_get(execution_tracker_t *tracker, execution_id_t exec_id);

/**
 * Release lock after execution_tracker_get()
 * @param tracker Tracker structure
 */
void execution_tracker_release(execution_tracker_t *tracker);

/**
 * Get executions by worker
 * @param tracker Tracker structure
 * @param worker_id Worker node ID
 * @param out_exec_ids Output array
 * @param max_count Maximum count
 * @return Number of executions returned
 */
size_t execution_tracker_get_by_worker(execution_tracker_t *tracker, node_id_t worker_id,
                                       execution_id_t *out_exec_ids, size_t max_count);

/**
 * Remove execution
 * @param tracker Tracker structure
 * @param exec_id Execution ID
 * @return 0 on success, error code on failure
 */
int execution_tracker_remove(execution_tracker_t *tracker, execution_id_t exec_id);

/**
 * Cleanup completed executions (older than threshold)
 * @param tracker Tracker structure
 * @return Number of executions cleaned up
 */
size_t execution_tracker_cleanup_completed(execution_tracker_t *tracker);

#endif // ROOLE_NODE_EXECUTION_TRACKER_H
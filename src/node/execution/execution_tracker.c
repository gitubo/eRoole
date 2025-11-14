// src/node/execution/execution_tracker.c
// Execution state tracking for distributed message processing

#define _POSIX_C_SOURCE 200809L

#include "roole/node/execution_tracker.h"
#include "roole/core/common.h"
#include <stdlib.h>
#include <string.h>

// ============================================================================
// HELPER: Find execution by ID
// ============================================================================

static execution_record_t* find_execution_unsafe(execution_tracker_t *tracker,
                                                 execution_id_t exec_id) {
    for (size_t i = 0; i < tracker->capacity; i++) {
        if (tracker->records[i].active && 
            tracker->records[i].exec_id == exec_id) {
            return &tracker->records[i];
        }
    }
    return NULL;
}

static execution_record_t* find_free_slot_unsafe(execution_tracker_t *tracker) {
    for (size_t i = 0; i < tracker->capacity; i++) {
        if (!tracker->records[i].active) {
            return &tracker->records[i];
        }
    }
    return NULL;
}

// ============================================================================
// PUBLIC API
// ============================================================================

int execution_tracker_init(execution_tracker_t *tracker, size_t capacity) {
    if (!tracker || capacity == 0) {
        LOG_ERROR("Invalid tracker init parameters");
        return RESULT_ERR_INVALID;
    }
    
    memset(tracker, 0, sizeof(execution_tracker_t));
    
    tracker->records = (execution_record_t*)safe_calloc(capacity, 
                                                         sizeof(execution_record_t));
    if (!tracker->records) {
        LOG_ERROR("Failed to allocate execution records");
        return RESULT_ERR_NOMEM;
    }
    
    tracker->capacity = capacity;
    tracker->next_exec_id = 1;  // Start from 1 (0 is invalid)
    
    if (pthread_rwlock_init(&tracker->lock, NULL) != 0) {
        LOG_ERROR("Failed to initialize tracker rwlock");
        safe_free(tracker->records);
        return RESULT_ERR_INVALID;
    }
    
    LOG_INFO("Execution tracker initialized (capacity: %zu)", capacity);
    return RESULT_OK;
}

void execution_tracker_destroy(execution_tracker_t *tracker) {
    if (!tracker) return;
    
    pthread_rwlock_wrlock(&tracker->lock);
    
    safe_free(tracker->records);
    tracker->records = NULL;
    tracker->capacity = 0;
    tracker->next_exec_id = 0;
    
    pthread_rwlock_unlock(&tracker->lock);
    pthread_rwlock_destroy(&tracker->lock);
    
    LOG_INFO("Execution tracker destroyed");
}

execution_id_t execution_tracker_add(execution_tracker_t *tracker,
                                     rule_id_t dag_id,
                                     node_id_t peer_id,
                                     const uint8_t *message,
                                     size_t message_len,
                                     uint8_t max_retries) {
    if (!tracker || !message || message_len == 0 || message_len > MAX_MESSAGE_SIZE) {
        LOG_ERROR("Invalid execution_tracker_add parameters");
        return 0;
    }
    
    pthread_rwlock_wrlock(&tracker->lock);
    
    // Find free slot
    execution_record_t *record = find_free_slot_unsafe(tracker);
    if (!record) {
        pthread_rwlock_unlock(&tracker->lock);
        LOG_ERROR("Execution tracker full (capacity: %zu)", tracker->capacity);
        return 0;
    }
    
    // Generate unique execution ID
    execution_id_t exec_id = tracker->next_exec_id++;
    
    // Initialize record
    memset(record, 0, sizeof(execution_record_t));
    record->exec_id = exec_id;
    record->dag_id = dag_id;
    record->assigned_peer = peer_id;
    record->status = EXEC_STATUS_PENDING;
    record->submit_time_ms = time_now_ms();
    record->start_time_ms = 0;
    record->complete_time_ms = 0;
    record->retry_count = 0;
    record->max_retries = max_retries;
    
    // Copy message data
    memcpy(record->message_data, message, message_len);
    record->message_len = message_len;
    
    record->active = 1;
    
    pthread_rwlock_unlock(&tracker->lock);
    
    LOG_DEBUG("Added execution: exec_id=%lu, dag_id=%u, peer=%u, msg_len=%zu",
              exec_id, dag_id, peer_id, message_len);
    
    return exec_id;
}

int execution_tracker_update_status(execution_tracker_t *tracker,
                                     execution_id_t exec_id,
                                     execution_status_t status) {
    if (!tracker || exec_id == 0) {
        return RESULT_ERR_INVALID;
    }
    
    pthread_rwlock_wrlock(&tracker->lock);
    
    execution_record_t *record = find_execution_unsafe(tracker, exec_id);
    if (!record) {
        pthread_rwlock_unlock(&tracker->lock);
        LOG_WARN("Execution %lu not found for status update", exec_id);
        return RESULT_ERR_NOTFOUND;
    }
    
    execution_status_t old_status = record->status;
    record->status = status;
    
    uint64_t now_ms = time_now_ms();
    
    // Update timestamps based on status transition
    switch (status) {
        case EXEC_STATUS_RUNNING:
            if (old_status == EXEC_STATUS_PENDING) {
                record->start_time_ms = now_ms;
            }
            break;
            
        case EXEC_STATUS_COMPLETED:
        case EXEC_STATUS_FAILED:
        case EXEC_STATUS_TIMEOUT:
            if (record->complete_time_ms == 0) {
                record->complete_time_ms = now_ms;
            }
            break;
            
        case EXEC_STATUS_RETRYING:
            record->retry_count++;
            LOG_DEBUG("Execution %lu retry %u/%u", 
                     exec_id, record->retry_count, record->max_retries);
            break;
            
        default:
            break;
    }
    
    pthread_rwlock_unlock(&tracker->lock);
    
    LOG_DEBUG("Updated execution %lu: %d → %d", exec_id, old_status, status);
    
    return RESULT_OK;
}

execution_record_t* execution_tracker_get(execution_tracker_t *tracker,
                                          execution_id_t exec_id) {
    if (!tracker || exec_id == 0) {
        return NULL;
    }
    
    pthread_rwlock_rdlock(&tracker->lock);
    
    execution_record_t *record = find_execution_unsafe(tracker, exec_id);
    
    if (!record) {
        pthread_rwlock_unlock(&tracker->lock);
        return NULL;
    }
    
    // Lock is held - caller MUST call execution_tracker_release()
    return record;
}

void execution_tracker_release(execution_tracker_t *tracker) {
    if (tracker) {
        pthread_rwlock_unlock(&tracker->lock);
    }
}

size_t execution_tracker_get_by_worker(execution_tracker_t *tracker,
                                        node_id_t worker_id,
                                        execution_id_t *out_exec_ids,
                                        size_t max_count) {
    if (!tracker || !out_exec_ids || max_count == 0) {
        return 0;
    }
    
    pthread_rwlock_rdlock(&tracker->lock);
    
    size_t found = 0;
    for (size_t i = 0; i < tracker->capacity && found < max_count; i++) {
        if (tracker->records[i].active &&
            tracker->records[i].assigned_peer == worker_id) {
            out_exec_ids[found++] = tracker->records[i].exec_id;
        }
    }
    
    pthread_rwlock_unlock(&tracker->lock);
    
    return found;
}

int execution_tracker_remove(execution_tracker_t *tracker,
                              execution_id_t exec_id) {
    if (!tracker || exec_id == 0) {
        return RESULT_ERR_INVALID;
    }
    
    pthread_rwlock_wrlock(&tracker->lock);
    
    execution_record_t *record = find_execution_unsafe(tracker, exec_id);
    if (!record) {
        pthread_rwlock_unlock(&tracker->lock);
        LOG_WARN("Execution %lu not found for removal", exec_id);
        return RESULT_ERR_NOTFOUND;
    }
    
    // Mark as inactive (slot can be reused)
    record->active = 0;
    
    pthread_rwlock_unlock(&tracker->lock);
    
    LOG_DEBUG("Removed execution %lu", exec_id);
    
    return RESULT_OK;
}

size_t execution_tracker_cleanup_completed(execution_tracker_t *tracker) {
    if (!tracker) return 0;
    
    pthread_rwlock_wrlock(&tracker->lock);
    
    uint64_t now_ms = time_now_ms();
    uint64_t cleanup_threshold_ms = 300000;  // 5 minutes
    
    size_t cleaned = 0;
    
    for (size_t i = 0; i < tracker->capacity; i++) {
        execution_record_t *record = &tracker->records[i];
        
        if (!record->active) continue;
        
        // Check if completed and old enough to clean up
        if ((record->status == EXEC_STATUS_COMPLETED ||
             record->status == EXEC_STATUS_FAILED ||
             record->status == EXEC_STATUS_TIMEOUT) &&
            record->complete_time_ms > 0 &&
            (now_ms - record->complete_time_ms) > cleanup_threshold_ms) {
            
            LOG_DEBUG("Cleaning up old execution: exec_id=%lu, status=%d, age=%lu ms",
                     record->exec_id, record->status, 
                     now_ms - record->complete_time_ms);
            
            record->active = 0;
            cleaned++;
        }
    }
    
    pthread_rwlock_unlock(&tracker->lock);
    
    if (cleaned > 0) {
        LOG_INFO("Cleaned up %zu completed executions", cleaned);
    }
    
    return cleaned;
}
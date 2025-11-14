// src/node/execution/node_executor.c
// Executor threads that process messages from the queue
// COMPLETE PRODUCTION IMPLEMENTATION

#define _POSIX_C_SOURCE 200809L

#include "roole/node/node_executor.h"
#include "roole/node/node_state.h"
#include "roole/dag/dag.h"
#include "roole/core/common.h"
#include "roole/core/event_bus.h"
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// FORWARD DECLARATIONS
// ============================================================================

static void publish_execution_event(node_state_t *state, 
                                    const message_t *msg,
                                    execution_status_t status);

// ============================================================================
// EXECUTOR THREAD FUNCTION
// ============================================================================

void* node_executor_thread_fn(void *arg) {
    node_state_t *state = (node_state_t*)arg;
    
    if (!state) {
        LOG_ERROR("Executor thread: NULL state");
        return NULL;
    }
    
    logger_push_component("executor");
    
    // Get subsystems
    message_queue_t *queue = node_state_get_message_queue(state);
    dag_catalog_t *catalog = node_state_get_dag_catalog(state);
    execution_tracker_t *exec_tracker = node_state_get_exec_tracker(state);
    event_bus_t *event_bus = node_state_get_event_bus(state);
    
    if (!queue || !catalog || !exec_tracker) {
        LOG_ERROR("Executor thread: Missing subsystems");
        logger_pop_component();
        return NULL;
    }
    
    LOG_INFO("Executor thread started");
    
    // Main processing loop
    while (!state->shutdown_flag) {
        message_t msg;
        
        // Pop message from queue (100ms timeout for shutdown check)
        int result = message_queue_pop(queue, &msg, 100);
        
        if (result == RESULT_ERR_TIMEOUT) {
            continue;  // Timeout - check shutdown and retry
        }
        
        if (result == RESULT_ERR_EMPTY) {
            continue;  // Queue empty (shouldn't happen with blocking pop)
        }
        
        if (result != RESULT_OK) {
            LOG_WARN("Failed to pop message from queue: %d", result);
            continue;
        }
        
        // Track active execution
        __sync_fetch_and_add(&state->active_executions, 1);
        
        LOG_DEBUG("Processing message: exec_id=%lu, dag_id=%u, sender=%u, msg_len=%zu",
                 msg.exec_id, msg.dag_id, msg.sender_id, msg.message_len);
        
        // Calculate queue wait time
        uint64_t now_ms = time_now_ms();
        uint64_t wait_time_ms = now_ms - msg.received_at_ms;
        
        if (state->histogram_queue_wait) {
            metrics_histogram_observe(state->histogram_queue_wait, (int)wait_time_ms);
        }
        
        // Update execution status to RUNNING
        execution_tracker_update_status(exec_tracker, msg.exec_id, 
                                       EXEC_STATUS_RUNNING);
        
        // Publish event
        if (event_bus) {
            publish_execution_event(state, &msg, EXEC_STATUS_RUNNING);
        }
        
        // Get DAG from catalog
        dag_t *dag = dag_catalog_get(catalog, msg.dag_id);
        
        if (!dag) {
            LOG_ERROR("DAG %u not found in catalog (exec_id=%lu)", 
                     msg.dag_id, msg.exec_id);
            
            execution_tracker_update_status(exec_tracker, msg.exec_id, 
                                           EXEC_STATUS_FAILED);
            
            if (event_bus) {
                publish_execution_event(state, &msg, EXEC_STATUS_FAILED);
            }
            
            __sync_fetch_and_sub(&state->active_executions, 1);
            __sync_fetch_and_add(&state->messages_failed, 1);
            
            if (state->metric_messages_failed) {
                metrics_counter_inc(state->metric_messages_failed);
            }
            continue;
        }
        
        // Prepare execution context
        dag_execution_context_t exec_ctx;
        memset(&exec_ctx, 0, sizeof(exec_ctx));
        
        exec_ctx.exec_id = msg.exec_id;
        exec_ctx.dag = dag;
        exec_ctx.input_data = msg.message_data;
        exec_ctx.input_len = msg.message_len;
        
        // Allocate output buffer
        exec_ctx.output_data = (uint8_t*)safe_malloc(MAX_MESSAGE_SIZE);
        if (!exec_ctx.output_data) {
            LOG_ERROR("Failed to allocate output buffer (exec_id=%lu)", msg.exec_id);
            
            dag_catalog_release(catalog);
            
            execution_tracker_update_status(exec_tracker, msg.exec_id, 
                                           EXEC_STATUS_FAILED);
            
            if (event_bus) {
                publish_execution_event(state, &msg, EXEC_STATUS_FAILED);
            }
            
            __sync_fetch_and_sub(&state->active_executions, 1);
            __sync_fetch_and_add(&state->messages_failed, 1);
            
            if (state->metric_messages_failed) {
                metrics_counter_inc(state->metric_messages_failed);
            }
            continue;
        }
        
        exec_ctx.output_capacity = MAX_MESSAGE_SIZE;
        exec_ctx.output_len = 0;
        
        // Execute DAG
        struct timespec exec_start;
        timespec_now(&exec_start);
        
        LOG_INFO("Executing DAG %u (exec_id=%lu, steps=%zu)", 
                msg.dag_id, msg.exec_id, dag->step_count);
        
        int exec_result = dag_execute(&exec_ctx);
        
        struct timespec exec_end;
        timespec_now(&exec_end);
        
        double duration_us = time_diff_us(&exec_start, &exec_end);
        int duration_ms = (int)(duration_us / 1000.0);
        
        // Release DAG catalog lock
        dag_catalog_release(catalog);
        
        // Update execution status and metrics
        if (exec_result == RESULT_OK) {
            execution_tracker_update_status(exec_tracker, msg.exec_id, 
                                           EXEC_STATUS_COMPLETED);
            
            if (event_bus) {
                publish_execution_event(state, &msg, EXEC_STATUS_COMPLETED);
            }
            
            __sync_fetch_and_add(&state->messages_processed, 1);
            
            LOG_INFO("Execution %lu completed successfully in %d ms (dag=%u, steps=%zu)",
                    msg.exec_id, duration_ms, msg.dag_id, dag->step_count);
            
            if (state->metric_messages_processed) {
                metrics_counter_inc(state->metric_messages_processed);
            }
        } else {
            execution_tracker_update_status(exec_tracker, msg.exec_id, 
                                           EXEC_STATUS_FAILED);
            
            if (event_bus) {
                publish_execution_event(state, &msg, EXEC_STATUS_FAILED);
            }
            
            __sync_fetch_and_add(&state->messages_failed, 1);
            
            LOG_ERROR("Execution %lu failed (dag=%u, error=%d, duration=%d ms)",
                     msg.exec_id, msg.dag_id, exec_result, duration_ms);
            
            if (state->metric_messages_failed) {
                metrics_counter_inc(state->metric_messages_failed);
            }
        }
        
        // Record execution duration histogram
        if (state->histogram_exec_duration) {
            metrics_histogram_observe(state->histogram_exec_duration, duration_ms);
        }
        
        // Record message size
        if (state->histogram_message_size) {
            metrics_histogram_observe(state->histogram_message_size, (int)msg.message_len);
        }
        
        // Cleanup
        safe_free(exec_ctx.output_data);
        __sync_fetch_and_sub(&state->active_executions, 1);
        
        LOG_DEBUG("Executor completed processing (active=%u)", 
                 state->active_executions);
    }
    
    LOG_INFO("Executor thread stopped gracefully");
    logger_pop_component();
    
    return NULL;
}

// ============================================================================
// HELPER: Publish Execution Events
// ============================================================================

static void publish_execution_event(node_state_t *state, 
                                    const message_t *msg,
                                    execution_status_t status) {
    if (!state || !msg) return;
    
    event_bus_t *event_bus = node_state_get_event_bus(state);
    if (!event_bus) return;
    
    event_t event;
    memset(&event, 0, sizeof(event));
    
    event.timestamp_ms = time_now_ms();
    event.source_node_id = state->identity.node_id;
    
    // Map execution status to event type
    switch (status) {
        case EXEC_STATUS_RUNNING:
            event.type = EVENT_TYPE_EXECUTION_STARTED;
            break;
        case EXEC_STATUS_COMPLETED:
            event.type = EVENT_TYPE_EXECUTION_COMPLETED;
            break;
        case EXEC_STATUS_FAILED:
        case EXEC_STATUS_TIMEOUT:
            event.type = EVENT_TYPE_EXECUTION_FAILED;
            break;
        default:
            return;  // Don't publish for other statuses
    }
    
    // Fill execution data
    event.data.execution.exec_id = msg->exec_id;
    event.data.execution.dag_id = msg->dag_id;
    event.data.execution.assigned_peer = state->identity.node_id;
    event.data.execution.timestamp_ms = event.timestamp_ms;
    event.data.execution.status_code = status;
    
    // Publish (non-blocking)
    if (event_bus_publish(event_bus, &event) != 0) {
        LOG_DEBUG("Failed to publish execution event (queue full?)");
    }
}

// ============================================================================
// EXECUTOR LIFECYCLE MANAGEMENT
// ============================================================================

int node_start_executors(node_state_t *state, size_t num_threads) {
    if (!state || num_threads == 0) {
        LOG_ERROR("Invalid executor start parameters");
        return RESULT_ERR_INVALID;
    }
    
    if (state->executor_threads) {
        LOG_WARN("Executors already started");
        return RESULT_ERR_EXISTS;
    }
    
    if (num_threads > 128) {
        LOG_WARN("Limiting executor threads from %zu to 128", num_threads);
        num_threads = 128;
    }
    
    LOG_INFO("Starting %zu executor threads...", num_threads);
    
    // Allocate thread handles
    state->executor_threads = (pthread_t*)safe_calloc(num_threads, 
                                                       sizeof(pthread_t));
    if (!state->executor_threads) {
        LOG_ERROR("Failed to allocate executor thread handles");
        return RESULT_ERR_NOMEM;
    }
    
    state->num_executor_threads = num_threads;
    
    // Start threads
    size_t started = 0;
    for (size_t i = 0; i < num_threads; i++) {
        int ret = pthread_create(&state->executor_threads[i], NULL,
                                node_executor_thread_fn, state);
        
        if (ret != 0) {
            LOG_ERROR("Failed to create executor thread %zu: %s", i, strerror(ret));
            break;
        }
        
        started++;
        LOG_DEBUG("Started executor thread %zu (tid=%lu)", 
                 i, (unsigned long)state->executor_threads[i]);
    }
    
    if (started < num_threads) {
        LOG_ERROR("Only started %zu/%zu executor threads", started, num_threads);
        
        // Stop already started threads
        state->shutdown_flag = 1;
        for (size_t j = 0; j < started; j++) {
            pthread_join(state->executor_threads[j], NULL);
        }
        
        safe_free(state->executor_threads);
        state->executor_threads = NULL;
        state->num_executor_threads = 0;
        state->shutdown_flag = 0;  // Reset
        
        return RESULT_ERR_INVALID;
    }
    
    LOG_INFO("All %zu executor threads started successfully", num_threads);
    
    return RESULT_OK;
}

void node_stop_executors(node_state_t *state) {
    if (!state || !state->executor_threads) {
        LOG_DEBUG("No executors to stop");
        return;
    }
    
    LOG_INFO("Stopping %zu executor threads...", state->num_executor_threads);
    
    // Signal shutdown (already set by node_state_shutdown)
    if (!state->shutdown_flag) {
        LOG_WARN("shutdown_flag not set, setting it now");
        state->shutdown_flag = 1;
    }
    
    // Wait for all threads to exit gracefully
    for (size_t i = 0; i < state->num_executor_threads; i++) {
        LOG_DEBUG("Waiting for executor thread %zu...", i);
        
        int ret = pthread_join(state->executor_threads[i], NULL);
        if (ret != 0) {
            LOG_ERROR("Failed to join executor thread %zu: %s", i, strerror(ret));
        } else {
            LOG_DEBUG("Executor thread %zu stopped", i);
        }
    }
    
    // Cleanup
    safe_free(state->executor_threads);
    state->executor_threads = NULL;
    state->num_executor_threads = 0;
    
    LOG_INFO("All executor threads stopped");
}

// ============================================================================
// EXECUTOR STATUS/STATISTICS
// ============================================================================

void node_executor_get_stats(node_state_t *state, 
                             size_t *out_active_threads,
                             uint32_t *out_active_executions) {
    if (!state) return;
    
    if (out_active_threads) {
        *out_active_threads = state->num_executor_threads;
    }
    
    if (out_active_executions) {
        *out_active_executions = state->active_executions;
    }
}
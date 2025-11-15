#ifndef ROOLE_NODE_EXECUTOR_H
#define ROOLE_NODE_EXECUTOR_H

#include "roole/node/node_state.h"

/**
 * Executor thread function
 * Pops messages from queue, updates status
 * @param arg Node state pointer
 * @return NULL
 */
void* node_executor_thread_fn(void *arg);

/**
 * Start executor threads
 * @param state Node state
 * @param num_threads Number of executor threads
 * @return 0 on success, error code on failure
 */
int node_start_executors(node_state_t *state, size_t num_threads);

/**
 * Stop executor threads
 * Waits for all threads to exit
 * @param state Node state
 */
void node_stop_executors(node_state_t *state);

/**
 * Get executor statistics
 * @param state Node state
 * @param out_active_threads Output: number of executor threads
 * @param out_active_executions Output: number of currently executing messages
 */
void node_executor_get_stats(node_state_t *state, 
                             size_t *out_active_threads,
                             uint32_t *out_active_executions);

#endif // ROOLE_NODE_EXECUTOR_H
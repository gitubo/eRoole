// include/roole/node/node_executor.h
// Message executor (processes messages from queue)

#ifndef ROOLE_NODE_EXECUTOR_H
#define ROOLE_NODE_EXECUTOR_H

#include "roole/node/node_state.h"

/**
 * Executor thread function
 * Pops messages from queue, executes DAGs, updates status
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

#endif // ROOLE_NODE_EXECUTOR_H
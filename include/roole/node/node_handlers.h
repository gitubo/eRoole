// include/roole/node/node_handlers.h
// RPC handler declarations (client and peer handlers)

#ifndef ROOLE_NODE_HANDLERS_H
#define ROOLE_NODE_HANDLERS_H

#include "roole/rpc/rpc_handler.h"
#include "roole/node/node_state.h"

/**
 * Build handler registry for node
 * Registers handlers based on node capabilities
 * @param state Node state
 * @return Handler registry, or NULL on error
 */
rpc_handler_registry_t* node_build_handler_registry(node_state_t *state);

// =============================================================================
// CLIENT HANDLERS (INGRESS channel - only if has_ingress capability)
// =============================================================================

/**
 * Handler: Submit message for processing
 * Request: [dag_id: 4 bytes][message: variable]
 * Response: [exec_id: 8 bytes][status: 1 byte]
 */
int handle_submit_message(const uint8_t *request, size_t request_len,
                          uint8_t **response, size_t *response_len,
                          void *user_context);

/**
 * Handler: Get execution status
 * Request: [exec_id: 8 bytes]
 * Response: [status: 1 byte]
 */
int handle_get_execution_status(const uint8_t *request, size_t request_len,
                                uint8_t **response, size_t *response_len,
                                void *user_context);

/**
 * Handler: List DAGs in catalog
 * Request: empty
 * Response: [count: 4 bytes][dag_id_1: 4 bytes]...[dag_id_n: 4 bytes]
 */
int handle_list_dags(const uint8_t *request, size_t request_len,
                    uint8_t **response, size_t *response_len,
                    void *user_context);

/**
 * Handler: Add DAG to catalog
 * Request: [dag_id: 4 bytes][name_len: 4 bytes][name][num_stages: 4 bytes][stages...]
 * Response: [ack: 1 byte]
 */
int handle_add_dag(const uint8_t *request, size_t request_len,
                  uint8_t **response, size_t *response_len,
                  void *user_context);

// =============================================================================
// PEER HANDLERS (DATA channel - always present)
// =============================================================================

/**
 * Handler: Process message on worker
 * Request: [exec_id: 8][dag_id: 4][sender_id: 2][message: variable]
 * Response: [ack: 1 byte]
 */
int handle_process_message(const uint8_t *request, size_t request_len,
                           uint8_t **response, size_t *response_len,
                           void *user_context);

/**
 * Handler: Execution status update
 * Request: [exec_id: 8][status: 1]
 * Response: [ack: 1 byte]
 */
int handle_execution_update(const uint8_t *request, size_t request_len,
                            uint8_t **response, size_t *response_len,
                            void *user_context);

/**
 * Handler: Sync DAG catalog
 * Request: [serialized DAG]
 * Response: [ack: 1 byte]
 */
int handle_sync_catalog(const uint8_t *request, size_t request_len,
                       uint8_t **response, size_t *response_len,
                       void *user_context);

#endif // ROOLE_NODE_HANDLERS_H
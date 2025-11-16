// include/roole/raft/raft_rpc.h
// Raft RPC handlers and serialization

#ifndef ROOLE_RAFT_RPC_H
#define ROOLE_RAFT_RPC_H

#include "roole/raft/raft_types.h"
#include "roole/raft/raft_state.h"
#include "roole/rpc/rpc_handler.h"
#include "roole/rpc/rpc_client.h"

// ============================================================================
// RPC FUNCTION IDS (add to rpc_types.h)
// ============================================================================

// Update to include/roole/rpc/rpc_types.h in the rpc_func_id_t enum:
//
// Add after FUNC_ID_DATASTORE_SYNC (line 34):
//    
//    // Raft consensus protocol
//    FUNC_ID_RAFT_REQUEST_VOTE   = 0x40,
//    FUNC_ID_RAFT_APPEND_ENTRIES = 0x41,
//    FUNC_ID_RAFT_INSTALL_SNAPSHOT = 0x42,

// ============================================================================
// RPC HANDLERS (Server-Side)
// ============================================================================

/**
 * Handle RequestVote RPC
 * Request: serialized raft_request_vote_req_t
 * Response: serialized raft_request_vote_resp_t
 */
int handle_raft_request_vote(const uint8_t *request,
                              size_t request_len,
                              uint8_t **response,
                              size_t *response_len,
                              void *user_context);

/**
 * Handle AppendEntries RPC
 * Request: serialized raft_append_entries_req_t
 * Response: serialized raft_append_entries_resp_t
 */
int handle_raft_append_entries(const uint8_t *request,
                                size_t request_len,
                                uint8_t **response,
                                size_t *response_len,
                                void *user_context);

/**
 * Handle InstallSnapshot RPC
 * Request: serialized raft_install_snapshot_req_t
 * Response: serialized raft_install_snapshot_resp_t
 */
int handle_raft_install_snapshot(const uint8_t *request,
                                  size_t request_len,
                                  uint8_t **response,
                                  size_t *response_len,
                                  void *user_context);

// ============================================================================
// HANDLER REGISTRATION
// ============================================================================

/**
 * Register Raft RPC handlers
 * @param registry Handler registry
 * @param raft_state Raft state (passed as user_context)
 * @return 0 on success, -1 on error
 */
int raft_register_handlers(rpc_handler_registry_t *registry,
                           raft_state_t *raft_state);

// ============================================================================
// RPC CLIENT (Peer Communication)
// ============================================================================

/**
 * Send RequestVote RPC to peer
 * @param client RPC client connection
 * @param req Request parameters
 * @param out_resp Output response
 * @param timeout_ms RPC timeout
 * @return RPC status code
 */
int raft_rpc_request_vote(rpc_client_t *client,
                          const raft_request_vote_req_t *req,
                          raft_request_vote_resp_t *out_resp,
                          int timeout_ms);

/**
 * Send AppendEntries RPC to peer
 * @param client RPC client connection
 * @param req Request parameters
 * @param out_resp Output response
 * @param timeout_ms RPC timeout
 * @return RPC status code
 */
int raft_rpc_append_entries(rpc_client_t *client,
                            const raft_append_entries_req_t *req,
                            raft_append_entries_resp_t *out_resp,
                            int timeout_ms);

/**
 * Send InstallSnapshot RPC to peer
 * @param client RPC client connection
 * @param req Request parameters
 * @param out_resp Output response
 * @param timeout_ms RPC timeout
 * @return RPC status code
 */
int raft_rpc_install_snapshot(rpc_client_t *client,
                              const raft_install_snapshot_req_t *req,
                              raft_install_snapshot_resp_t *out_resp,
                              int timeout_ms);

// ============================================================================
// SERIALIZATION
// ============================================================================

// RequestVote serialization
size_t raft_serialize_request_vote_req(const raft_request_vote_req_t *req,
                                        uint8_t *buffer,
                                        size_t buffer_size);

int raft_deserialize_request_vote_req(const uint8_t *buffer,
                                       size_t buffer_len,
                                       raft_request_vote_req_t *req);

size_t raft_serialize_request_vote_resp(const raft_request_vote_resp_t *resp,
                                         uint8_t *buffer,
                                         size_t buffer_size);

int raft_deserialize_request_vote_resp(const uint8_t *buffer,
                                        size_t buffer_len,
                                        raft_request_vote_resp_t *resp);

// AppendEntries serialization
size_t raft_serialize_append_entries_req(const raft_append_entries_req_t *req,
                                          uint8_t *buffer,
                                          size_t buffer_size);

int raft_deserialize_append_entries_req(const uint8_t *buffer,
                                         size_t buffer_len,
                                         raft_append_entries_req_t *req);

size_t raft_serialize_append_entries_resp(const raft_append_entries_resp_t *resp,
                                           uint8_t *buffer,
                                           size_t buffer_size);

int raft_deserialize_append_entries_resp(const uint8_t *buffer,
                                          size_t buffer_len,
                                          raft_append_entries_resp_t *resp);

// InstallSnapshot serialization
size_t raft_serialize_install_snapshot_req(const raft_install_snapshot_req_t *req,
                                            uint8_t *buffer,
                                            size_t buffer_size);

int raft_deserialize_install_snapshot_req(const uint8_t *buffer,
                                           size_t buffer_len,
                                           raft_install_snapshot_req_t *req);

size_t raft_serialize_install_snapshot_resp(const raft_install_snapshot_resp_t *resp,
                                             uint8_t *buffer,
                                             size_t buffer_size);

int raft_deserialize_install_snapshot_resp(const uint8_t *buffer,
                                            size_t buffer_len,
                                            raft_install_snapshot_resp_t *resp);

// Log entry serialization (for AppendEntries)
size_t raft_serialize_log_entry(const raft_log_entry_t *entry,
                                 uint8_t *buffer,
                                 size_t buffer_size);

int raft_deserialize_log_entry(const uint8_t *buffer,
                                size_t buffer_len,
                                raft_log_entry_t *entry);

#endif // ROOLE_RAFT_RPC_H
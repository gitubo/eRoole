// include/roole/raft/raft_state.h
// Raft state machine core interface

#ifndef ROOLE_RAFT_STATE_H
#define ROOLE_RAFT_STATE_H

#include "roole/raft/raft_types.h"
#include "roole/cluster/cluster_view.h"
#include "roole/rpc/rpc_client.h"
#include "roole/rpc/rpc_handler.h"
#include <stdint.h>

// Forward declarations
typedef struct raft_state raft_state_t;

// ============================================================================
// STATE MACHINE CALLBACK
// ============================================================================

/**
 * State machine apply callback
 * Called when a log entry is committed and ready to apply
 * @param entry Log entry to apply
 * @param user_data User context
 * @return 0 on success, error code on failure
 */
typedef int (*raft_apply_fn)(const raft_log_entry_t *entry, void *user_data);

/**
 * Snapshot create callback
 * Called when Raft wants to create a snapshot
 * @param last_included_index Index of last entry in snapshot
 * @param last_included_term Term of last entry
 * @param out_data Output snapshot data (allocated by callback)
 * @param out_len Output snapshot length
 * @param user_data User context
 * @return 0 on success
 */
typedef int (*raft_snapshot_create_fn)(uint64_t last_included_index,
                                       uint64_t last_included_term,
                                       uint8_t **out_data,
                                       size_t *out_len,
                                       void *user_data);

/**
 * Snapshot restore callback
 * Called when receiving a snapshot from leader
 * @param data Snapshot data
 * @param len Snapshot length
 * @param last_included_index Last index in snapshot
 * @param last_included_term Last term in snapshot
 * @param user_data User context
 * @return 0 on success
 */
typedef int (*raft_snapshot_restore_fn)(const uint8_t *data,
                                        size_t len,
                                        uint64_t last_included_index,
                                        uint64_t last_included_term,
                                        void *user_data);

typedef struct raft_callbacks {
    raft_apply_fn on_apply;
    raft_snapshot_create_fn on_snapshot_create;
    raft_snapshot_restore_fn on_snapshot_restore;
    void *user_data;
} raft_callbacks_t;

// ============================================================================
// LIFECYCLE
// ============================================================================

/**
 * Create Raft state machine
 * @param my_id This node's ID
 * @param cluster_view Shared cluster view (for peer discovery)
 * @param config Raft configuration
 * @param callbacks State machine callbacks
 * @return Raft state handle, or NULL on error
 */
raft_state_t* raft_state_create(node_id_t my_id,
                                 cluster_view_t *cluster_view,
                                 const raft_config_t *config,
                                 const raft_callbacks_t *callbacks);

/**
 * Start Raft state machine
 * Starts election timer and background threads
 * @param state Raft state
 * @return 0 on success, -1 on error
 */
int raft_state_start(raft_state_t *state);

/**
 * Stop Raft state machine
 * @param state Raft state
 */
void raft_state_stop(raft_state_t *state);

/**
 * Destroy Raft state machine
 * @param state Raft state
 */
void raft_state_destroy(raft_state_t *state);

// ============================================================================
// CLIENT REQUEST INTERFACE
// ============================================================================

/**
 * Submit command to Raft cluster
 * If this node is leader, appends to log and starts replication
 * If follower, returns error with leader hint
 * @param state Raft state
 * @param data Command data
 * @param data_len Command length
 * @param out_index Output log index (if successful)
 * @param out_term Output term (if successful)
 * @return 0 if accepted, -1 if not leader, other error codes
 */
int raft_submit_command(raft_state_t *state,
                        const uint8_t *data,
                        size_t data_len,
                        uint64_t *out_index,
                        uint64_t *out_term);

/**
 * Wait for log entry to be committed
 * Blocks until entry at index is committed or timeout
 * @param state Raft state
 * @param index Log index to wait for
 * @param timeout_ms Timeout in milliseconds
 * @return 0 if committed, -1 on timeout/error
 */
int raft_wait_committed(raft_state_t *state,
                        uint64_t index,
                        int timeout_ms);

// ============================================================================
// PEER MANAGEMENT
// ============================================================================

/**
 * Add peer connection
 * Called when cluster_view detects new peer
 * @param state Raft state
 * @param peer_id Peer node ID
 * @param peer_ip Peer IP address
 * @param peer_port Peer RPC port
 * @return 0 on success
 */
int raft_add_peer(raft_state_t *state,
                  node_id_t peer_id,
                  const char *peer_ip,
                  uint16_t peer_port);

/**
 * Remove peer connection
 * Called when cluster_view detects peer failure
 * @param state Raft state
 * @param peer_id Peer node ID
 * @return 0 on success
 */
int raft_remove_peer(raft_state_t *state, node_id_t peer_id);

// ============================================================================
// STATUS AND INTROSPECTION
// ============================================================================

/**
 * Get current Raft state (follower/candidate/leader)
 * @param state Raft state
 * @return Current role
 */
raft_state_t raft_get_state(raft_state_t *state);

/**
 * Get current term
 * @param state Raft state
 * @return Current term
 */
uint64_t raft_get_term(raft_state_t *state);

/**
 * Get current leader ID
 * @param state Raft state
 * @return Leader node ID (0 if unknown)
 */
node_id_t raft_get_leader(raft_state_t *state);

/**
 * Get commit index
 * @param state Raft state
 * @return Commit index
 */
uint64_t raft_get_commit_index(raft_state_t *state);

/**
 * Get last applied index
 * @param state Raft state
 * @return Last applied index
 */
uint64_t raft_get_last_applied(raft_state_t *state);

/**
 * Get statistics
 * @param state Raft state
 * @param out_stats Output statistics
 */
void raft_get_stats(raft_state_t *state, raft_stats_t *out_stats);

/**
 * Check if this node is leader
 * @param state Raft state
 * @return 1 if leader, 0 otherwise
 */
int raft_is_leader(raft_state_t *state);

/**
 * Dump Raft state for debugging
 * @param state Raft state
 * @param label Debug label
 */
void raft_dump_state(raft_state_t *state, const char *label);

#endif // ROOLE_RAFT_STATE_H
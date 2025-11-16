// include/roole/raft/raft_state.h
// Raft state machine core interface

#ifndef ROOLE_RAFT_STATE_H
#define ROOLE_RAFT_STATE_H

#include "roole/raft/raft_types.h"
#include "roole/raft/raft_operational_metrics.h"
#include "roole/cluster/cluster_view.h"
#include "roole/rpc/rpc_client.h"
#include "roole/rpc/rpc_handler.h"
#include <stdint.h>

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
// INTERNAL STRUCTURE
// ============================================================================

typedef struct raft_state {
    node_id_t my_id;
    cluster_view_t *cluster_view;
    raft_config_t config;
    
    // State components
    raft_persistent_state_t *persistent;
    raft_volatile_state_t *volatile_state;
    raft_leader_state_t *leader_state;
    raft_snapshot_t *snapshot;
    
    // Callbacks
    raft_callbacks_t callbacks;
    
    // RPC clients (for peer communication)
    rpc_client_t *peer_clients[RAFT_MAX_PEERS];
    pthread_mutex_t peers_lock;
    
    // Background threads
    pthread_t election_timer_thread;
    pthread_t heartbeat_thread;
    pthread_t apply_thread;
    
    // Statistics
    raft_stats_t stats;
    raft_operational_metrics_t *op_metrics;
    
    // Shutdown flag
    volatile int shutdown;
} raft_state_t;

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

// ============================================================================
// INTERNAL HELPERS (for RPC handlers)
// ============================================================================

/**
 * Reset election timer
 * Should be called when receiving valid AppendEntries or granting vote
 * @param state Raft state
 */
void raft_reset_election_timer(raft_state_t *state);

/**
 * Check if candidate's log is at least as up-to-date as ours
 * Used in RequestVote RPC handling (§5.4.1)
 * @param state Raft state
 * @param candidate_last_index Candidate's last log index
 * @param candidate_last_term Candidate's last log term
 * @return 1 if candidate's log is up-to-date, 0 otherwise
 */
int raft_is_log_up_to_date(raft_state_t *state, 
                            uint64_t candidate_last_index,
                            uint64_t candidate_last_term);

/**
 * Check if log contains entry at given index with given term
 * Used in AppendEntries RPC handling (§5.3)
 * @param state Raft state
 * @param index Log index to check
 * @param term Expected term
 * @return 1 if log contains matching entry, 0 otherwise
 */
int raft_log_contains_entry(raft_state_t *state, 
                             uint64_t index, 
                             uint64_t term);

/**
 * Append entries to log, truncating conflicts
 * Used in AppendEntries RPC handling (§5.3)
 * @param state Raft state
 * @param entries Entries to append
 * @param count Number of entries
 * @param prev_index Index immediately before new entries
 * @return 0 on success, -1 on error
 */
int raft_append_log_entries(raft_state_t *state, 
                             const raft_log_entry_t *entries,
                             size_t count, 
                             uint64_t prev_index);

/**
 * Transition to follower state
 * Used when discovering higher term
 * @param state Raft state
 * @param term Term to transition to
 */
void raft_become_follower(raft_state_t *state, uint64_t term);

/**
 * Get last log index
 * @param state Raft state
 * @return Last log index (or snapshot_last_index if log is empty)
 */
uint64_t raft_get_last_log_index(raft_state_t *state);

/**
 * Get last log term
 * @param state Raft state
 * @return Last log term (or snapshot_last_term if log is empty)
 */
uint64_t raft_get_last_log_term(raft_state_t *state);

#endif // ROOLE_RAFT_STATE_H
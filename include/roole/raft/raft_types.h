// include/roole/raft/raft_types.h
// Core Raft consensus data structures and types

#ifndef ROOLE_RAFT_TYPES_H
#define ROOLE_RAFT_TYPES_H

#include "roole/core/common.h"
#include <stdint.h>
#include <pthread.h>

// ============================================================================
// CONFIGURATION
// ============================================================================

#define RAFT_MAX_LOG_ENTRIES 10000
#define RAFT_MAX_PEERS 16
#define RAFT_SNAPSHOT_THRESHOLD 5000

// Timeouts (in milliseconds)
#define RAFT_ELECTION_TIMEOUT_MIN 150
#define RAFT_ELECTION_TIMEOUT_MAX 300
#define RAFT_HEARTBEAT_INTERVAL 50
#define RAFT_RPC_TIMEOUT 100

// ============================================================================
// RAFT NODE STATE
// ============================================================================

typedef enum {
    RAFT_STATE_FOLLOWER = 0,
    RAFT_STATE_CANDIDATE = 1,
    RAFT_STATE_LEADER = 2
} raft_state_t;

// ============================================================================
// LOG ENTRY
// ============================================================================

typedef enum {
    RAFT_ENTRY_COMMAND = 0,      // Normal client command
    RAFT_ENTRY_CONFIGURATION = 1, // Cluster configuration change
    RAFT_ENTRY_NOOP = 2          // No-op (leader election)
} raft_entry_type_t;

typedef struct raft_log_entry {
    uint64_t term;               // Term when entry was received
    uint64_t index;              // Position in log
    raft_entry_type_t type;      // Entry type
    
    // Command data (for RAFT_ENTRY_COMMAND)
    uint8_t *data;               // Command payload
    size_t data_len;             // Command length
    
    // Metadata
    uint64_t timestamp_ms;       // When entry was created
    node_id_t client_id;         // Which client submitted (0 for internal)
} raft_log_entry_t;

// ============================================================================
// RPC MESSAGE STRUCTURES
// ============================================================================

// RequestVote RPC
typedef struct {
    uint64_t term;               // Candidate's term
    node_id_t candidate_id;      // Candidate requesting vote
    uint64_t last_log_index;     // Index of candidate's last log entry
    uint64_t last_log_term;      // Term of candidate's last log entry
} raft_request_vote_req_t;

typedef struct {
    uint64_t term;               // Current term, for candidate to update itself
    int vote_granted;            // True if candidate received vote
} raft_request_vote_resp_t;

// AppendEntries RPC
typedef struct {
    uint64_t term;               // Leader's term
    node_id_t leader_id;         // So follower can redirect clients
    uint64_t prev_log_index;     // Index of log entry immediately preceding new ones
    uint64_t prev_log_term;      // Term of prevLogIndex entry
    uint64_t leader_commit;      // Leader's commitIndex
    
    // Entries to append (empty for heartbeat)
    raft_log_entry_t *entries;   // Log entries to store (NULL for heartbeat)
    size_t entry_count;          // Number of entries
} raft_append_entries_req_t;

typedef struct {
    uint64_t term;               // Current term, for leader to update itself
    int success;                 // True if follower contained entry matching prevLogIndex/prevLogTerm
    uint64_t match_index;        // Highest index known to be replicated (optimization)
} raft_append_entries_resp_t;

// InstallSnapshot RPC (for catch-up)
typedef struct {
    uint64_t term;               // Leader's term
    node_id_t leader_id;         // So follower can redirect clients
    uint64_t last_included_index; // Snapshot replaces all entries up through this index
    uint64_t last_included_term; // Term of lastIncludedIndex
    size_t offset;               // Byte offset where chunk is positioned in snapshot file
    uint8_t *data;               // Raw bytes of snapshot chunk
    size_t data_len;             // Length of data
    int done;                    // True if this is the last chunk
} raft_install_snapshot_req_t;

typedef struct {
    uint64_t term;               // Current term, for leader to update itself
} raft_install_snapshot_resp_t;

// ============================================================================
// PERSISTENT STATE (must survive crashes)
// ============================================================================

typedef struct raft_persistent_state {
    uint64_t current_term;       // Latest term server has seen
    node_id_t voted_for;         // CandidateId that received vote in current term (0 = none)
    
    // Log
    raft_log_entry_t *log;       // Log entries (dynamically allocated)
    size_t log_capacity;         // Capacity of log array
    size_t log_count;            // Number of entries in log
    
    // Snapshot metadata
    uint64_t snapshot_last_index;
    uint64_t snapshot_last_term;
    
    pthread_rwlock_t lock;       // Protects persistent state
} raft_persistent_state_t;

// ============================================================================
// VOLATILE STATE (all servers)
// ============================================================================

typedef struct raft_volatile_state {
    uint64_t commit_index;       // Index of highest log entry known to be committed
    uint64_t last_applied;       // Index of highest log entry applied to state machine
    
    raft_state_t state;          // Current role (follower/candidate/leader)
    node_id_t current_leader;    // Current leader (0 if unknown)
    
    // Election timing
    uint64_t election_timeout_ms; // Randomized election timeout
    uint64_t last_heartbeat_ms;  // When we last heard from leader
    
    pthread_mutex_t lock;        // Protects volatile state
} raft_volatile_state_t;

// ============================================================================
// VOLATILE STATE (leaders only)
// ============================================================================

typedef struct raft_leader_state {
    // For each server, index of next log entry to send
    uint64_t next_index[RAFT_MAX_PEERS];
    
    // For each server, index of highest log entry known to be replicated
    uint64_t match_index[RAFT_MAX_PEERS];
    
    // Peer tracking
    node_id_t peers[RAFT_MAX_PEERS];  // Peer node IDs
    size_t peer_count;                // Number of peers
    
    uint64_t last_heartbeat_sent_ms;  // When we last sent heartbeats
    
    pthread_mutex_t lock;             // Protects leader state
} raft_leader_state_t;

// ============================================================================
// SNAPSHOT
// ============================================================================

typedef struct raft_snapshot {
    uint64_t last_included_index;
    uint64_t last_included_term;
    
    uint8_t *data;
    size_t data_len;
    
    pthread_mutex_t lock;
} raft_snapshot_t;

// ============================================================================
// RAFT CONFIGURATION
// ============================================================================

typedef struct raft_config {
    // Timing
    uint32_t election_timeout_min_ms;
    uint32_t election_timeout_max_ms;
    uint32_t heartbeat_interval_ms;
    uint32_t rpc_timeout_ms;
    
    // Thresholds
    size_t snapshot_threshold;       // Create snapshot after this many log entries
    size_t max_entries_per_append;   // Max entries in single AppendEntries RPC
    
    // Persistence
    int enable_persistence;          // Whether to persist to disk
    char persistence_dir[256];       // Directory for persistent state
} raft_config_t;

// Default configuration
static inline raft_config_t raft_default_config(void) {
    return (raft_config_t){
        .election_timeout_min_ms = RAFT_ELECTION_TIMEOUT_MIN,
        .election_timeout_max_ms = RAFT_ELECTION_TIMEOUT_MAX,
        .heartbeat_interval_ms = RAFT_HEARTBEAT_INTERVAL,
        .rpc_timeout_ms = RAFT_RPC_TIMEOUT,
        .snapshot_threshold = RAFT_SNAPSHOT_THRESHOLD,
        .max_entries_per_append = 100,
        .enable_persistence = 1,
        .persistence_dir = "/tmp/raft"
    };
}

// ============================================================================
// STATISTICS
// ============================================================================

typedef struct raft_stats {
    // Election stats
    uint64_t elections_started;
    uint64_t elections_won;
    uint64_t votes_received;
    uint64_t votes_rejected;
    
    // Replication stats
    uint64_t append_entries_sent;
    uint64_t append_entries_received;
    uint64_t append_entries_success;
    uint64_t append_entries_failed;
    
    // Client requests
    uint64_t commands_received;
    uint64_t commands_committed;
    uint64_t commands_applied;
    
    // Snapshots
    uint64_t snapshots_created;
    uint64_t snapshots_installed;
    
    // State transitions
    uint64_t became_follower;
    uint64_t became_candidate;
    uint64_t became_leader;
    
    pthread_mutex_t lock;
} raft_stats_t;

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

const char* raft_state_to_string(raft_state_t state);

#endif // ROOLE_RAFT_TYPES_H
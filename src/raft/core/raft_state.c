// src/raft/core/raft_state.c
// Core Raft consensus state machine implementation

#define _POSIX_C_SOURCE 200809L

#include "roole/raft/raft_state.h"
#include "roole/raft/raft_rpc.h"
#include "roole/core/common.h"
#include "roole/logger/logger.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

// ============================================================================
// INTERNAL STRUCTURE
// ============================================================================

struct raft_state {
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
    
    // Shutdown flag
    volatile int shutdown;
};

// ============================================================================
// HELPER: Random Election Timeout
// ============================================================================

static uint64_t random_election_timeout(const raft_config_t *config) {
    uint32_t range = config->election_timeout_max_ms - config->election_timeout_min_ms;
    uint32_t offset = rand() % range;
    return config->election_timeout_min_ms + offset;
}

// ============================================================================
// HELPER: Log Operations
// ============================================================================

static uint64_t get_last_log_index(raft_state_t *state) {
    pthread_rwlock_rdlock(&state->persistent->lock);
    
    if (state->persistent->log_count > 0) {
        uint64_t idx = state->persistent->log[state->persistent->log_count - 1].index;
        pthread_rwlock_unlock(&state->persistent->lock);
        return idx;
    }
    
    // If no entries, return snapshot last index
    uint64_t idx = state->persistent->snapshot_last_index;
    pthread_rwlock_unlock(&state->persistent->lock);
    return idx;
}

static uint64_t get_last_log_term(raft_state_t *state) {
    pthread_rwlock_rdlock(&state->persistent->lock);
    
    if (state->persistent->log_count > 0) {
        uint64_t term = state->persistent->log[state->persistent->log_count - 1].term;
        pthread_rwlock_unlock(&state->persistent->lock);
        return term;
    }
    
    uint64_t term = state->persistent->snapshot_last_term;
    pthread_rwlock_unlock(&state->persistent->lock);
    return term;
}

static int is_log_up_to_date(raft_state_t *state, uint64_t candidate_last_index,
                             uint64_t candidate_last_term) {
    uint64_t our_last_index = get_last_log_index(state);
    uint64_t our_last_term = get_last_log_term(state);
    
    // Candidate's log is more up-to-date if last log term is greater,
    // or terms are equal but index is >= ours
    if (candidate_last_term > our_last_term) {
        return 1;
    }
    if (candidate_last_term == our_last_term && candidate_last_index >= our_last_index) {
        return 1;
    }
    return 0;
}

// ============================================================================
// HELPER: State Transitions
// ============================================================================

static void become_follower(raft_state_t *state, uint64_t term) {
    pthread_mutex_lock(&state->volatile_state->lock);
    
    if (state->volatile_state->state != RAFT_STATE_FOLLOWER) {
        LOG_INFO("Raft: Becoming FOLLOWER (term=%lu)", term);
        state->stats.became_follower++;
    }
    
    state->volatile_state->state = RAFT_STATE_FOLLOWER;
    state->volatile_state->current_leader = 0;
    
    pthread_rwlock_wrlock(&state->persistent->lock);
    state->persistent->current_term = term;
    state->persistent->voted_for = 0;
    pthread_rwlock_unlock(&state->persistent->lock);
    
    pthread_mutex_unlock(&state->volatile_state->lock);
}

static void become_candidate(raft_state_t *state) {
    pthread_mutex_lock(&state->volatile_state->lock);
    
    LOG_INFO("Raft: Becoming CANDIDATE");
    state->volatile_state->state = RAFT_STATE_CANDIDATE;
    state->volatile_state->current_leader = 0;
    state->stats.became_candidate++;
    
    pthread_mutex_unlock(&state->volatile_state->lock);
}

static void become_leader(raft_state_t *state) {
    pthread_mutex_lock(&state->volatile_state->lock);
    
    LOG_INFO("Raft: Becoming LEADER");
    state->volatile_state->state = RAFT_STATE_LEADER;
    state->volatile_state->current_leader = state->my_id;
    state->stats.became_leader++;
    
    // Initialize leader state
    pthread_mutex_lock(&state->leader_state->lock);
    
    uint64_t last_log_idx = get_last_log_index(state);
    
    for (size_t i = 0; i < state->leader_state->peer_count; i++) {
        state->leader_state->next_index[i] = last_log_idx + 1;
        state->leader_state->match_index[i] = 0;
    }
    
    state->leader_state->last_heartbeat_sent_ms = time_now_ms();
    
    pthread_mutex_unlock(&state->leader_state->lock);
    pthread_mutex_unlock(&state->volatile_state->lock);
    
    // Append no-op entry to commit entries from previous terms
    uint8_t noop = 0;
    uint64_t idx, term;
    raft_submit_command(state, &noop, 0, &idx, &term);
}

// ============================================================================
// ELECTION TIMER THREAD
// ============================================================================

static void reset_election_timer(raft_state_t *state) {
    pthread_mutex_lock(&state->volatile_state->lock);
    state->volatile_state->last_heartbeat_ms = time_now_ms();
    state->volatile_state->election_timeout_ms = random_election_timeout(&state->config);
    pthread_mutex_unlock(&state->volatile_state->lock);
}

static int should_start_election(raft_state_t *state) {
    pthread_mutex_lock(&state->volatile_state->lock);
    
    if (state->volatile_state->state == RAFT_STATE_LEADER) {
        pthread_mutex_unlock(&state->volatile_state->lock);
        return 0;
    }
    
    uint64_t now = time_now_ms();
    uint64_t elapsed = now - state->volatile_state->last_heartbeat_ms;
    uint64_t timeout = state->volatile_state->election_timeout_ms;
    
    pthread_mutex_unlock(&state->volatile_state->lock);
    
    return (elapsed > timeout);
}

static void start_election(raft_state_t *state) {
    // Transition to candidate
    become_candidate(state);
    
    // Increment term and vote for self
    pthread_rwlock_wrlock(&state->persistent->lock);
    state->persistent->current_term++;
    state->persistent->voted_for = state->my_id;
    uint64_t term = state->persistent->current_term;
    pthread_rwlock_unlock(&state->persistent->lock);
    
    uint64_t last_log_index = get_last_log_index(state);
    uint64_t last_log_term = get_last_log_term(state);
    
    // Reset election timer
    reset_election_timer(state);
    
    state->stats.elections_started++;
    
    LOG_INFO("Raft: Starting election for term %lu", term);
    
    // Count votes (start with self-vote)
    int votes = 1;
    size_t majority = (state->leader_state->peer_count + 1) / 2 + 1;
    
    LOG_DEBUG("Raft: Need %zu/%zu votes for majority", majority, state->leader_state->peer_count + 1);
    
    // Send RequestVote RPCs to all peers
    pthread_mutex_lock(&state->peers_lock);
    
    for (size_t i = 0; i < state->leader_state->peer_count; i++) {
        node_id_t peer_id = state->leader_state->peers[i];
        rpc_client_t *client = state->peer_clients[i];
        
        if (!client) continue;
        
        // Build request
        raft_request_vote_req_t req = {
            .term = term,
            .candidate_id = state->my_id,
            .last_log_index = last_log_index,
            .last_log_term = last_log_term
        };
        
        // Send RPC (synchronous for simplicity)
        raft_request_vote_resp_t resp;
        int status = raft_rpc_request_vote(client, &req, &resp, state->config.rpc_timeout_ms);
        
        if (status != RPC_STATUS_SUCCESS) {
            LOG_WARN("Raft: RequestVote to peer %u failed", peer_id);
            continue;
        }
        
        // Check if still candidate and same term
        pthread_mutex_lock(&state->volatile_state->lock);
        int still_candidate = (state->volatile_state->state == RAFT_STATE_CANDIDATE);
        pthread_mutex_unlock(&state->volatile_state->lock);
        
        pthread_rwlock_rdlock(&state->persistent->lock);
        int same_term = (state->persistent->current_term == term);
        pthread_rwlock_unlock(&state->persistent->lock);
        
        if (!still_candidate || !same_term) {
            break;
        }
        
        // If peer has higher term, step down
        if (resp.term > term) {
            LOG_INFO("Raft: Peer %u has higher term %lu, stepping down", peer_id, resp.term);
            become_follower(state, resp.term);
            break;
        }
        
        // Count vote
        if (resp.vote_granted) {
            votes++;
            state->stats.votes_received++;
            LOG_INFO("Raft: Received vote from peer %u (%d/%zu)", peer_id, votes, majority);
            
            if ((size_t)votes >= majority) {
                LOG_INFO("Raft: Won election with %d/%zu votes", votes, majority);
                state->stats.elections_won++;
                become_leader(state);
                break;
            }
        } else {
            state->stats.votes_rejected++;
        }
    }
    
    pthread_mutex_unlock(&state->peers_lock);
}

static void* election_timer_thread_fn(void *arg) {
    raft_state_t *state = (raft_state_t*)arg;
    
    logger_push_component("raft:election");
    LOG_INFO("Raft election timer started");
    
    while (!state->shutdown) {
        if (should_start_election(state)) {
            start_election(state);
        }
        usleep(10000); // Check every 10ms
    }
    
    LOG_INFO("Raft election timer stopped");
    logger_pop_component();
    return NULL;
}

// ============================================================================
// HEARTBEAT/REPLICATION THREAD
// ============================================================================

static void send_append_entries_to_peer(raft_state_t *state, size_t peer_idx) {
    node_id_t peer_id = state->leader_state->peers[peer_idx];
    rpc_client_t *client = state->peer_clients[peer_idx];
    
    if (!client) return;
    
    pthread_rwlock_rdlock(&state->persistent->lock);
    uint64_t term = state->persistent->current_term;
    uint64_t next_idx = state->leader_state->next_index[peer_idx];
    uint64_t commit_idx = state->volatile_state->commit_index;
    
    // Get prev log entry info
    uint64_t prev_log_index = next_idx - 1;
    uint64_t prev_log_term = 0;
    
    if (prev_log_index > 0 && prev_log_index <= state->persistent->log_count) {
        prev_log_term = state->persistent->log[prev_log_index - 1].term;
    }
    
    // Get entries to send (empty for heartbeat)
    size_t entry_count = 0;
    raft_log_entry_t *entries = NULL;
    
    // TODO: Actually send log entries if next_idx < last_log_idx
    
    pthread_rwlock_unlock(&state->persistent->lock);
    
    // Build AppendEntries request
    raft_append_entries_req_t req = {
        .term = term,
        .leader_id = state->my_id,
        .prev_log_index = prev_log_index,
        .prev_log_term = prev_log_term,
        .leader_commit = commit_idx,
        .entries = entries,
        .entry_count = entry_count
    };
    
    // Send RPC
    raft_append_entries_resp_t resp;
    int status = raft_rpc_append_entries(client, &req, &resp, state->config.rpc_timeout_ms);
    
    state->stats.append_entries_sent++;
    
    if (status != RPC_STATUS_SUCCESS) {
        LOG_WARN("Raft: AppendEntries to peer %u failed", peer_id);
        return;
    }
    
    // Process response
    if (resp.term > term) {
        LOG_INFO("Raft: Peer %u has higher term, stepping down", peer_id);
        become_follower(state, resp.term);
        return;
    }
    
    if (resp.success) {
        state->stats.append_entries_success++;
        
        // Update nextIndex and matchIndex
        pthread_mutex_lock(&state->leader_state->lock);
        state->leader_state->next_index[peer_idx] = resp.match_index + 1;
        state->leader_state->match_index[peer_idx] = resp.match_index;
        pthread_mutex_unlock(&state->leader_state->lock);
    } else {
        state->stats.append_entries_failed++;
        
        // Decrement nextIndex and retry
        pthread_mutex_lock(&state->leader_state->lock);
        if (state->leader_state->next_index[peer_idx] > 1) {
            state->leader_state->next_index[peer_idx]--;
        }
        pthread_mutex_unlock(&state->leader_state->lock);
    }
}

static void* heartbeat_thread_fn(void *arg) {
    raft_state_t *state = (raft_state_t*)arg;
    
    logger_push_component("raft:heartbeat");
    LOG_INFO("Raft heartbeat thread started");
    
    while (!state->shutdown) {
        pthread_mutex_lock(&state->volatile_state->lock);
        int is_leader = (state->volatile_state->state == RAFT_STATE_LEADER);
        pthread_mutex_unlock(&state->volatile_state->lock);
        
        if (is_leader) {
            pthread_mutex_lock(&state->peers_lock);
            
            for (size_t i = 0; i < state->leader_state->peer_count; i++) {
                send_append_entries_to_peer(state, i);
            }
            
            pthread_mutex_unlock(&state->peers_lock);
        }
        
        usleep(state->config.heartbeat_interval_ms * 1000);
    }
    
    LOG_INFO("Raft heartbeat thread stopped");
    logger_pop_component();
    return NULL;
}

// ============================================================================
// APPLY THREAD
// ============================================================================

static void* apply_thread_fn(void *arg) {
    raft_state_t *state = (raft_state_t*)arg;
    
    logger_push_component("raft:apply");
    LOG_INFO("Raft apply thread started");
    
    while (!state->shutdown) {
        pthread_mutex_lock(&state->volatile_state->lock);
        uint64_t last_applied = state->volatile_state->last_applied;
        uint64_t commit_index = state->volatile_state->commit_index;
        pthread_mutex_unlock(&state->volatile_state->lock);
        
        if (commit_index > last_applied) {
            // Apply committed entries
            for (uint64_t i = last_applied + 1; i <= commit_index; i++) {
                pthread_rwlock_rdlock(&state->persistent->lock);
                
                if (i > state->persistent->log_count) {
                    pthread_rwlock_unlock(&state->persistent->lock);
                    break;
                }
                
                raft_log_entry_t *entry = &state->persistent->log[i - 1];
                
                // Apply to state machine
                if (state->callbacks.on_apply) {
                    state->callbacks.on_apply(entry, state->callbacks.user_data);
                    state->stats.commands_applied++;
                }
                
                pthread_rwlock_unlock(&state->persistent->lock);
                
                // Update lastApplied
                pthread_mutex_lock(&state->volatile_state->lock);
                state->volatile_state->last_applied = i;
                pthread_mutex_unlock(&state->volatile_state->lock);
            }
        }
        
        usleep(10000); // Check every 10ms
    }
    
    LOG_INFO("Raft apply thread stopped");
    logger_pop_component();
    return NULL;
}

// ============================================================================
// PUBLIC API: LIFECYCLE
// ============================================================================

raft_state_t* raft_state_create(node_id_t my_id,
                                 cluster_view_t *cluster_view,
                                 const raft_config_t *config,
                                 const raft_callbacks_t *callbacks) {
    if (!cluster_view || !config || !callbacks) {
        LOG_ERROR("Invalid Raft create parameters");
        return NULL;
    }
    
    raft_state_t *state = safe_calloc(1, sizeof(raft_state_t));
    if (!state) return NULL;
    
    state->my_id = my_id;
    state->cluster_view = cluster_view;
    state->config = *config;
    state->callbacks = *callbacks;
    state->shutdown = 0;
    
    // Allocate persistent state
    state->persistent = safe_calloc(1, sizeof(raft_persistent_state_t));
    state->persistent->current_term = 0;
    state->persistent->voted_for = 0;
    state->persistent->log = safe_calloc(RAFT_MAX_LOG_ENTRIES, sizeof(raft_log_entry_t));
    state->persistent->log_capacity = RAFT_MAX_LOG_ENTRIES;
    state->persistent->log_count = 0;
    pthread_rwlock_init(&state->persistent->lock, NULL);
    
    // Allocate volatile state
    state->volatile_state = safe_calloc(1, sizeof(raft_volatile_state_t));
    state->volatile_state->commit_index = 0;
    state->volatile_state->last_applied = 0;
    state->volatile_state->state = RAFT_STATE_FOLLOWER;
    state->volatile_state->election_timeout_ms = random_election_timeout(config);
    state->volatile_state->last_heartbeat_ms = time_now_ms();
    pthread_mutex_init(&state->volatile_state->lock, NULL);
    
    // Allocate leader state
    state->leader_state = safe_calloc(1, sizeof(raft_leader_state_t));
    pthread_mutex_init(&state->leader_state->lock, NULL);
    
    // Allocate snapshot
    state->snapshot = safe_calloc(1, sizeof(raft_snapshot_t));
    pthread_mutex_init(&state->snapshot->lock, NULL);
    
    // Initialize stats
    pthread_mutex_init(&state->stats.lock, NULL);
    pthread_mutex_init(&state->peers_lock, NULL);
    
    LOG_INFO("Raft state created (node_id=%u)", my_id);
    return state;
}

int raft_state_start(raft_state_t *state) {
    if (!state) return -1;
    
    LOG_INFO("Starting Raft state machine...");
    
    // Start election timer
    if (pthread_create(&state->election_timer_thread, NULL, election_timer_thread_fn, state) != 0) {
        LOG_ERROR("Failed to start election timer thread");
        return -1;
    }
    
    // Start heartbeat thread
    if (pthread_create(&state->heartbeat_thread, NULL, heartbeat_thread_fn, state) != 0) {
        LOG_ERROR("Failed to start heartbeat thread");
        state->shutdown = 1;
        pthread_join(state->election_timer_thread, NULL);
        return -1;
    }
    
    // Start apply thread
    if (pthread_create(&state->apply_thread, NULL, apply_thread_fn, state) != 0) {
        LOG_ERROR("Failed to start apply thread");
        state->shutdown = 1;
        pthread_join(state->election_timer_thread, NULL);
        pthread_join(state->heartbeat_thread, NULL);
        return -1;
    }
    
    LOG_INFO("Raft state machine started");
    return 0;
}

void raft_state_stop(raft_state_t *state) {
    if (!state) return;
    
    LOG_INFO("Stopping Raft state machine...");
    state->shutdown = 1;
    
    pthread_join(state->election_timer_thread, NULL);
    pthread_join(state->heartbeat_thread, NULL);
    pthread_join(state->apply_thread, NULL);
    
    LOG_INFO("Raft state machine stopped");
}

void raft_state_destroy(raft_state_t *state) {
    if (!state) return;
    
    // Cleanup
    if (state->persistent) {
        pthread_rwlock_destroy(&state->persistent->lock);
        safe_free(state->persistent->log);
        safe_free(state->persistent);
    }
    
    if (state->volatile_state) {
        pthread_mutex_destroy(&state->volatile_state->lock);
        safe_free(state->volatile_state);
    }
    
    if (state->leader_state) {
        pthread_mutex_destroy(&state->leader_state->lock);
        safe_free(state->leader_state);
    }
    
    if (state->snapshot) {
        pthread_mutex_destroy(&state->snapshot->lock);
        safe_free(state->snapshot);
    }
    
    pthread_mutex_destroy(&state->stats.lock);
    pthread_mutex_destroy(&state->peers_lock);
    
    safe_free(state);
    LOG_INFO("Raft state machine destroyed");
}

// ============================================================================
// PUBLIC API: CLIENT REQUESTS
// ============================================================================

int raft_submit_command(raft_state_t *state,
                        const uint8_t *data,
                        size_t data_len,
                        uint64_t *out_index,
                        uint64_t *out_term) {
    if (!state || !data) return -1;
    
    // Check if leader
    pthread_mutex_lock(&state->volatile_state->lock);
    if (state->volatile_state->state != RAFT_STATE_LEADER) {
        pthread_mutex_unlock(&state->volatile_state->lock);
        return -1; // Not leader
    }
    pthread_mutex_unlock(&state->volatile_state->lock);
    
    // Append to log
    pthread_rwlock_wrlock(&state->persistent->lock);
    
    if (state->persistent->log_count >= state->persistent->log_capacity) {
        pthread_rwlock_unlock(&state->persistent->lock);
        return -1; // Log full
    }
    
    uint64_t term = state->persistent->current_term;
    uint64_t index = get_last_log_index(state) + 1;
    
    raft_log_entry_t *entry = &state->persistent->log[state->persistent->log_count];
    entry->term = term;
    entry->index = index;
    entry->type = RAFT_ENTRY_COMMAND;
    entry->data = safe_malloc(data_len);
    memcpy(entry->data, data, data_len);
    entry->data_len = data_len;
    entry->timestamp_ms = time_now_ms();
    entry->client_id = 0;
    
    state->persistent->log_count++;
    
    pthread_rwlock_unlock(&state->persistent->lock);
    
    if (out_index) *out_index = index;
    if (out_term) *out_term = term;
    
    state->stats.commands_received++;
    
    LOG_DEBUG("Raft: Command appended at index=%lu term=%lu", index, term);
    
    return 0;
}

int raft_wait_committed(raft_state_t *state, uint64_t index, int timeout_ms) {
    uint64_t start = time_now_ms();
    
    while (1) {
        pthread_mutex_lock(&state->volatile_state->lock);
        uint64_t commit_idx = state->volatile_state->commit_index;
        pthread_mutex_unlock(&state->volatile_state->lock);
        
        if (commit_idx >= index) {
            return 0;
        }
        
        if (timeout_ms > 0) {
            uint64_t elapsed = time_now_ms() - start;
            if (elapsed > (uint64_t)timeout_ms) {
                return -1; // Timeout
            }
        }
        
        usleep(10000); // 10ms
    }
}

// ============================================================================
// HELPERS
// ============================================================================

const char* raft_state_to_string(raft_state_t state) {
    switch (state) {
        case RAFT_STATE_FOLLOWER: return "FOLLOWER";
        case RAFT_STATE_CANDIDATE: return "CANDIDATE";
        case RAFT_STATE_LEADER: return "LEADER";
        default: return "UNKNOWN";
    }
}

int raft_add_peer(raft_state_t *state,
                  node_id_t peer_id,
                  const char *peer_ip,
                  uint16_t peer_port) {
    if (!state || !peer_ip || peer_port == 0) {
        LOG_ERROR("Invalid raft_add_peer parameters");
        return -1;
    }
    
    pthread_mutex_lock(&state->peers_lock);
    
    // Check if peer already exists
    for (size_t i = 0; i < state->leader_state->peer_count; i++) {
        if (state->leader_state->peers[i] == peer_id) {
            pthread_mutex_unlock(&state->peers_lock);
            LOG_DEBUG("Raft: Peer %u already exists", peer_id);
            return 0;
        }
    }
    
    // Check capacity
    if (state->leader_state->peer_count >= RAFT_MAX_PEERS) {
        pthread_mutex_unlock(&state->peers_lock);
        LOG_ERROR("Raft: Max peers reached (%d)", RAFT_MAX_PEERS);
        return -1;
    }
    
    // Add peer
    size_t idx = state->leader_state->peer_count;
    state->leader_state->peers[idx] = peer_id;
    
    // Create RPC client for this peer
    state->peer_clients[idx] = rpc_client_connect(peer_ip, peer_port,
                                                   RPC_CHANNEL_DATA, 8192);
    
    if (!state->peer_clients[idx]) {
        pthread_mutex_unlock(&state->peers_lock);
        LOG_ERROR("Raft: Failed to connect to peer %u (%s:%u)", 
                 peer_id, peer_ip, peer_port);
        return -1;
    }
    
    // Initialize leader state for this peer
    pthread_mutex_lock(&state->leader_state->lock);
    uint64_t next_idx = get_last_log_index(state) + 1;
    state->leader_state->next_index[idx] = next_idx;
    state->leader_state->match_index[idx] = 0;
    pthread_mutex_unlock(&state->leader_state->lock);
    
    state->leader_state->peer_count++;
    
    pthread_mutex_unlock(&state->peers_lock);
    
    LOG_INFO("Raft: Added peer %u (%s:%u), total peers: %zu",
             peer_id, peer_ip, peer_port, state->leader_state->peer_count);
    
    return 0;
}

int raft_remove_peer(raft_state_t *state, node_id_t peer_id) {
    if (!state) return -1;
    
    pthread_mutex_lock(&state->peers_lock);
    
    // Find peer
    int found = -1;
    for (size_t i = 0; i < state->leader_state->peer_count; i++) {
        if (state->leader_state->peers[i] == peer_id) {
            found = (int)i;
            break;
        }
    }
    
    if (found < 0) {
        pthread_mutex_unlock(&state->peers_lock);
        LOG_DEBUG("Raft: Peer %u not found", peer_id);
        return 0;
    }
    
    // Close RPC client
    if (state->peer_clients[found]) {
        rpc_client_close(state->peer_clients[found]);
        state->peer_clients[found] = NULL;
    }
    
    // Remove from array (shift remaining peers)
    size_t idx = (size_t)found;
    if (idx < state->leader_state->peer_count - 1) {
        // Shift peers
        memmove(&state->leader_state->peers[idx],
                &state->leader_state->peers[idx + 1],
                (state->leader_state->peer_count - idx - 1) * sizeof(node_id_t));
        
        // Shift RPC clients
        memmove(&state->peer_clients[idx],
                &state->peer_clients[idx + 1],
                (state->leader_state->peer_count - idx - 1) * sizeof(rpc_client_t*));
        
        // Shift leader state
        pthread_mutex_lock(&state->leader_state->lock);
        memmove(&state->leader_state->next_index[idx],
                &state->leader_state->next_index[idx + 1],
                (state->leader_state->peer_count - idx - 1) * sizeof(uint64_t));
        memmove(&state->leader_state->match_index[idx],
                &state->leader_state->match_index[idx + 1],
                (state->leader_state->peer_count - idx - 1) * sizeof(uint64_t));
        pthread_mutex_unlock(&state->leader_state->lock);
    }
    
    state->leader_state->peer_count--;
    
    pthread_mutex_unlock(&state->peers_lock);
    
    LOG_INFO("Raft: Removed peer %u, remaining peers: %zu",
             peer_id, state->leader_state->peer_count);
    
    return 0;
}

// Add accessor functions (already declared in raft_state.h):

int raft_is_leader(raft_state_t *state) {
    if (!state) return 0;
    
    pthread_mutex_lock(&state->volatile_state->lock);
    int is_leader = (state->volatile_state->state == RAFT_STATE_LEADER);
    pthread_mutex_unlock(&state->volatile_state->lock);
    
    return is_leader;
}

uint64_t raft_get_term(raft_state_t *state) {
    if (!state) return 0;
    
    pthread_rwlock_rdlock(&state->persistent->lock);
    uint64_t term = state->persistent->current_term;
    pthread_rwlock_unlock(&state->persistent->lock);
    
    return term;
}

node_id_t raft_get_leader(raft_state_t *state) {
    if (!state) return 0;
    
    pthread_mutex_lock(&state->volatile_state->lock);
    node_id_t leader = state->volatile_state->current_leader;
    pthread_mutex_unlock(&state->volatile_state->lock);
    
    return leader;
}

uint64_t raft_get_commit_index(raft_state_t *state) {
    if (!state) return 0;
    
    pthread_mutex_lock(&state->volatile_state->lock);
    uint64_t commit = state->volatile_state->commit_index;
    pthread_mutex_unlock(&state->volatile_state->lock);
    
    return commit;
}

uint64_t raft_get_last_applied(raft_state_t *state) {
    if (!state) return 0;
    
    pthread_mutex_lock(&state->volatile_state->lock);
    uint64_t last_applied = state->volatile_state->last_applied;
    pthread_mutex_unlock(&state->volatile_state->lock);
    
    return last_applied;
}

void raft_get_stats(raft_state_t *state, raft_stats_t *out_stats) {
    if (!state || !out_stats) return;
    
    pthread_mutex_lock(&state->stats.lock);
    *out_stats = state->stats;
    pthread_mutex_unlock(&state->stats.lock);
}
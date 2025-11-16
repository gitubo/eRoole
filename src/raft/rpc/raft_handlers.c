// src/raft/rpc/raft_handlers.c
// Raft RPC request handlers (server-side)

#define _POSIX_C_SOURCE 200809L

#include "roole/raft/raft_rpc.h"
#include "roole/raft/raft_state.h"
#include "roole/rpc/rpc_types.h"
#include "roole/core/common.h"
#include "roole/logger/logger.h"
#include <stdlib.h>
#include <string.h>

// Forward declarations from raft_state.c (internal functions)
extern void reset_election_timer(raft_state_t *state);
extern int is_log_up_to_date(raft_state_t *state, uint64_t candidate_last_index,
                             uint64_t candidate_last_term);
extern int log_contains_entry(raft_state_t *state, uint64_t index, uint64_t term);
extern void append_log_entries(raft_state_t *state, const raft_log_entry_t *entries,
                               size_t count, uint64_t prev_index);
extern void become_follower(raft_state_t *state, uint64_t term);

// ============================================================================
// HANDLER: RequestVote RPC
// ============================================================================

/**
 * Handle RequestVote RPC
 * Raft §5.2: Receiver implementation:
 * 1. Reply false if term < currentTerm
 * 2. If votedFor is null or candidateId, and candidate's log is at least
 *    as up-to-date as receiver's log, grant vote
 */
int handle_raft_request_vote(const uint8_t *request,
                              size_t request_len,
                              uint8_t **response,
                              size_t *response_len,
                              void *user_context) {
    if (!request || !response || !response_len || !user_context) {
        LOG_ERROR("Invalid RequestVote handler parameters");
        return RPC_STATUS_BAD_ARGUMENT;
    }
    
    raft_state_t *state = (raft_state_t*)user_context;
    
    // Deserialize request
    raft_request_vote_req_t req;
    if (raft_deserialize_request_vote_req(request, request_len, &req) != 0) {
        LOG_ERROR("Failed to deserialize RequestVote request");
        return RPC_STATUS_BAD_ARGUMENT;
    }
    
    LOG_INFO("Raft: Received RequestVote from candidate %u (term=%lu)",
             req.candidate_id, req.term);
    
    // Prepare response
    raft_request_vote_resp_t resp = {0};
    
    // Get current state
    pthread_rwlock_wrlock(&state->persistent->lock);
    
    uint64_t current_term = state->persistent->current_term;
    node_id_t voted_for = state->persistent->voted_for;
    
    // Rule 1: If RPC request term > currentTerm, update term and convert to follower
    if (req.term > current_term) {
        LOG_INFO("Raft: Candidate %u has higher term %lu > %lu, updating term",
                 req.candidate_id, req.term, current_term);
        
        state->persistent->current_term = req.term;
        state->persistent->voted_for = 0;
        current_term = req.term;
        voted_for = 0;
        
        pthread_rwlock_unlock(&state->persistent->lock);
        become_follower(state, req.term);
        pthread_rwlock_wrlock(&state->persistent->lock);
    }
    
    resp.term = state->persistent->current_term;
    resp.vote_granted = 0;
    
    // Rule 2: Reply false if term < currentTerm
    if (req.term < current_term) {
        LOG_INFO("Raft: Rejecting vote for %u (term %lu < %lu)",
                 req.candidate_id, req.term, current_term);
        pthread_rwlock_unlock(&state->persistent->lock);
        goto send_response;
    }
    
    // Rule 3: Grant vote if:
    // - Haven't voted this term, OR already voted for this candidate
    // - Candidate's log is at least as up-to-date as ours
    int can_vote = (voted_for == 0 || voted_for == req.candidate_id);
    
    if (can_vote) {
        pthread_rwlock_unlock(&state->persistent->lock);
        
        int log_ok = is_log_up_to_date(state, req.last_log_index, req.last_log_term);
        
        pthread_rwlock_wrlock(&state->persistent->lock);
        
        if (log_ok) {
            // Grant vote
            state->persistent->voted_for = req.candidate_id;
            resp.vote_granted = 1;
            
            pthread_rwlock_unlock(&state->persistent->lock);
            
            // Reset election timer (we just voted for someone)
            reset_election_timer(state);
            
            LOG_INFO("Raft: Granted vote to candidate %u (term=%lu)",
                     req.candidate_id, req.term);
        } else {
            pthread_rwlock_unlock(&state->persistent->lock);
            
            LOG_INFO("Raft: Rejecting vote for %u (log not up-to-date)",
                     req.candidate_id);
        }
    } else {
        pthread_rwlock_unlock(&state->persistent->lock);
        
        LOG_INFO("Raft: Rejecting vote for %u (already voted for %u)",
                 req.candidate_id, voted_for);
    }
    
send_response:
    // Serialize response
    *response = safe_malloc(16);
    if (!*response) {
        return RPC_STATUS_INTERNAL_ERROR;
    }
    
    *response_len = raft_serialize_request_vote_resp(&resp, *response, 16);
    
    if (*response_len == 0) {
        safe_free(*response);
        return RPC_STATUS_INTERNAL_ERROR;
    }
    
    return RPC_STATUS_SUCCESS;
}

// ============================================================================
// HANDLER: AppendEntries RPC
// ============================================================================

/**
 * Handle AppendEntries RPC
 * Raft §5.3: Receiver implementation:
 * 1. Reply false if term < currentTerm
 * 2. Reply false if log doesn't contain entry at prevLogIndex with prevLogTerm
 * 3. If existing entry conflicts, delete it and all following
 * 4. Append new entries not already in log
 * 5. If leaderCommit > commitIndex, set commitIndex = min(leaderCommit, index of last new entry)
 */
int handle_raft_append_entries(const uint8_t *request,
                                size_t request_len,
                                uint8_t **response,
                                size_t *response_len,
                                void *user_context) {
    if (!request || !response || !response_len || !user_context) {
        LOG_ERROR("Invalid AppendEntries handler parameters");
        return RPC_STATUS_BAD_ARGUMENT;
    }
    
    raft_state_t *state = (raft_state_t*)user_context;
    
    // Deserialize request
    raft_append_entries_req_t req;
    if (raft_deserialize_append_entries_req(request, request_len, &req) != 0) {
        LOG_ERROR("Failed to deserialize AppendEntries request");
        return RPC_STATUS_BAD_ARGUMENT;
    }
    
    if (req.entry_count == 0) {
        LOG_DEBUG("Raft: Received heartbeat from leader %u (term=%lu, commit=%lu)",
                  req.leader_id, req.term, req.leader_commit);
    } else {
        LOG_INFO("Raft: Received AppendEntries from leader %u (term=%lu, entries=%zu)",
                 req.leader_id, req.term, req.entry_count);
    }
    
    // Prepare response
    raft_append_entries_resp_t resp = {0};
    
    // Get current state
    pthread_rwlock_wrlock(&state->persistent->lock);
    
    uint64_t current_term = state->persistent->current_term;
    
    // Rule 1: If RPC term > currentTerm, update term and convert to follower
    if (req.term > current_term) {
        LOG_INFO("Raft: Leader %u has higher term %lu > %lu, updating term",
                 req.leader_id, req.term, current_term);
        
        state->persistent->current_term = req.term;
        state->persistent->voted_for = 0;
        current_term = req.term;
        
        pthread_rwlock_unlock(&state->persistent->lock);
        become_follower(state, req.term);
        pthread_rwlock_wrlock(&state->persistent->lock);
    }
    
    resp.term = state->persistent->current_term;
    resp.success = 0;
    resp.match_index = 0;
    
    // Rule 2: Reply false if term < currentTerm
    if (req.term < current_term) {
        LOG_INFO("Raft: Rejecting AppendEntries from %u (term %lu < %lu)",
                 req.leader_id, req.term, current_term);
        pthread_rwlock_unlock(&state->persistent->lock);
        goto send_response;
    }
    
    // Valid leader - reset election timer
    pthread_rwlock_unlock(&state->persistent->lock);
    reset_election_timer(state);
    
    // Update current leader
    pthread_mutex_lock(&state->volatile_state->lock);
    state->volatile_state->current_leader = req.leader_id;
    pthread_mutex_unlock(&state->volatile_state->lock);
    
    pthread_rwlock_wrlock(&state->persistent->lock);
    
    // Rule 3: Reply false if log doesn't contain entry at prevLogIndex with prevLogTerm
    if (!log_contains_entry(state, req.prev_log_index, req.prev_log_term)) {
        LOG_DEBUG("Raft: Log inconsistency at prev_index=%lu prev_term=%lu",
                  req.prev_log_index, req.prev_log_term);
        resp.success = 0;
        pthread_rwlock_unlock(&state->persistent->lock);
        goto send_response;
    }
    
    // Rule 4 & 5: Append entries
    if (req.entry_count > 0) {
        append_log_entries(state, req.entries, req.entry_count, req.prev_log_index);
        
        LOG_INFO("Raft: Appended %zu entries starting at index %lu",
                 req.entry_count, req.prev_log_index + 1);
    }
    
    // Calculate match index
    resp.match_index = req.prev_log_index + req.entry_count;
    
    // Rule 6: Update commitIndex
    if (req.leader_commit > state->volatile_state->commit_index) {
        uint64_t new_commit = ROOLE_MIN(req.leader_commit, resp.match_index);
        
        pthread_mutex_lock(&state->volatile_state->lock);
        uint64_t old_commit = state->volatile_state->commit_index;
        state->volatile_state->commit_index = new_commit;
        pthread_mutex_unlock(&state->volatile_state->lock);
        
        if (new_commit > old_commit) {
            LOG_INFO("Raft: Updated commit index: %lu -> %lu",
                     old_commit, new_commit);
        }
    }
    
    resp.success = 1;
    
    pthread_rwlock_unlock(&state->persistent->lock);
    
send_response:
    // Cleanup request
    raft_free_append_entries_req(&req);
    
    // Serialize response
    *response = safe_malloc(32);
    if (!*response) {
        return RPC_STATUS_INTERNAL_ERROR;
    }
    
    *response_len = raft_serialize_append_entries_resp(&resp, *response, 32);
    
    if (*response_len == 0) {
        safe_free(*response);
        return RPC_STATUS_INTERNAL_ERROR;
    }
    
    return RPC_STATUS_SUCCESS;
}

// ============================================================================
// HANDLER: InstallSnapshot RPC
// ============================================================================

/**
 * Handle InstallSnapshot RPC
 * Raft §7: Receiver implementation:
 * 1. Reply immediately if term < currentTerm
 * 2. Create new snapshot file if first chunk (offset is 0)
 * 3. Write data into snapshot file at given offset
 * 4. Reply and wait for more data chunks
 * 5. Save snapshot file, discard any existing snapshot with smaller index
 * 6. If existing log entry has same index and term as snapshot's last entry,
 *    retain log entries following it and reply
 * 7. Discard entire log
 * 8. Reset state machine using snapshot contents
 */
int handle_raft_install_snapshot(const uint8_t *request,
                                  size_t request_len,
                                  uint8_t **response,
                                  size_t *response_len,
                                  void *user_context) {
    if (!request || !response || !response_len || !user_context) {
        LOG_ERROR("Invalid InstallSnapshot handler parameters");
        return RPC_STATUS_BAD_ARGUMENT;
    }
    
    raft_state_t *state = (raft_state_t*)user_context;
    
    // Deserialize request
    raft_install_snapshot_req_t req;
    if (raft_deserialize_install_snapshot_req(request, request_len, &req) != 0) {
        LOG_ERROR("Failed to deserialize InstallSnapshot request");
        return RPC_STATUS_BAD_ARGUMENT;
    }
    
    LOG_INFO("Raft: Received InstallSnapshot from leader %u (term=%lu, last_idx=%lu, offset=%zu, done=%d)",
             req.leader_id, req.term, req.last_included_index, req.offset, req.done);
    
    // Prepare response
    raft_install_snapshot_resp_t resp = {0};
    
    // Get current state
    pthread_rwlock_wrlock(&state->persistent->lock);
    
    uint64_t current_term = state->persistent->current_term;
    
    // Update term if needed
    if (req.term > current_term) {
        state->persistent->current_term = req.term;
        state->persistent->voted_for = 0;
        
        pthread_rwlock_unlock(&state->persistent->lock);
        become_follower(state, req.term);
        pthread_rwlock_wrlock(&state->persistent->lock);
    }
    
    resp.term = state->persistent->current_term;
    
    // Rule 1: Reply immediately if term < currentTerm
    if (req.term < state->persistent->current_term) {
        LOG_INFO("Raft: Rejecting InstallSnapshot from %u (term %lu < %lu)",
                 req.leader_id, req.term, state->persistent->current_term);
        pthread_rwlock_unlock(&state->persistent->lock);
        goto send_response;
    }
    
    pthread_rwlock_unlock(&state->persistent->lock);
    
    // Reset election timer
    reset_election_timer(state);
    
    // Update current leader
    pthread_mutex_lock(&state->volatile_state->lock);
    state->volatile_state->current_leader = req.leader_id;
    pthread_mutex_unlock(&state->volatile_state->lock);
    
    // TODO: For multi-chunk snapshots, accumulate chunks
    // For now, assume single-chunk snapshots (done=true)
    
    if (req.done) {
        // Install snapshot via callback
        if (state->callbacks.on_snapshot_restore) {
            int result = state->callbacks.on_snapshot_restore(
                req.data, req.data_len,
                req.last_included_index, req.last_included_term,
                state->callbacks.user_data
            );
            
            if (result != 0) {
                LOG_ERROR("Raft: Failed to restore snapshot");
                goto send_response;
            }
        }
        
        // Update snapshot metadata
        pthread_rwlock_wrlock(&state->persistent->lock);
        state->persistent->snapshot_last_index = req.last_included_index;
        state->persistent->snapshot_last_term = req.last_included_term;
        pthread_rwlock_unlock(&state->persistent->lock);
        
        // Update commit index and last applied
        pthread_mutex_lock(&state->volatile_state->lock);
        if (req.last_included_index > state->volatile_state->commit_index) {
            state->volatile_state->commit_index = req.last_included_index;
        }
        if (req.last_included_index > state->volatile_state->last_applied) {
            state->volatile_state->last_applied = req.last_included_index;
        }
        pthread_mutex_unlock(&state->volatile_state->lock);
        
        LOG_INFO("Raft: Snapshot installed successfully (last_idx=%lu)",
                 req.last_included_index);
    }
    
send_response:
    // Cleanup request
    raft_free_install_snapshot_req(&req);
    
    // Serialize response
    *response = safe_malloc(16);
    if (!*response) {
        return RPC_STATUS_INTERNAL_ERROR;
    }
    
    *response_len = raft_serialize_install_snapshot_resp(&resp, *response, 16);
    
    if (*response_len == 0) {
        safe_free(*response);
        return RPC_STATUS_INTERNAL_ERROR;
    }
    
    return RPC_STATUS_SUCCESS;
}

// ============================================================================
// HANDLER REGISTRATION
// ============================================================================

/**
 * Register all Raft RPC handlers
 */
int raft_register_handlers(rpc_handler_registry_t *registry,
                           raft_state_t *raft_state) {
    if (!registry || !raft_state) {
        LOG_ERROR("Invalid handler registration parameters");
        return -1;
    }
    
    LOG_INFO("Registering Raft RPC handlers...");
    
    // Register RequestVote handler
    if (rpc_handler_register(registry, FUNC_ID_RAFT_REQUEST_VOTE,
                            handle_raft_request_vote, raft_state) != 0) {
        LOG_ERROR("Failed to register RequestVote handler");
        return -1;
    }
    
    // Register AppendEntries handler
    if (rpc_handler_register(registry, FUNC_ID_RAFT_APPEND_ENTRIES,
                            handle_raft_append_entries, raft_state) != 0) {
        LOG_ERROR("Failed to register AppendEntries handler");
        return -1;
    }
    
    // Register InstallSnapshot handler
    if (rpc_handler_register(registry, FUNC_ID_RAFT_INSTALL_SNAPSHOT,
                            handle_raft_install_snapshot, raft_state) != 0) {
        LOG_ERROR("Failed to register InstallSnapshot handler");
        return -1;
    }
    
    LOG_INFO("Raft RPC handlers registered successfully");
    return 0;
}
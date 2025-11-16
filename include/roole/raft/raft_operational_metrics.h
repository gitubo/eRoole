// include/roole/raft/raft_operational_metrics.h
// CLEAR SEPARATION: Operational metrics vs internal consensus state

#ifndef ROOLE_RAFT_OPERATIONAL_METRICS_H
#define ROOLE_RAFT_OPERATIONAL_METRICS_H

#include "roole/raft/raft_types.h"
#include <stdatomic.h>

// ============================================================================
// OPERATIONAL METRICS (Actionable for Operators)
// These are what ops teams need to monitor cluster health
// ============================================================================

typedef struct raft_operational_metrics {
    // Leadership & Availability
    _Atomic int is_leader;              // 1 if leader, 0 otherwise (boolean)
    _Atomic uint64_t leader_election_ts;// Timestamp of last election
    _Atomic uint32_t elections_total;   // Total elections started
    
    // Replication Health (Leader-only)
    _Atomic uint32_t followers_healthy; // Number of followers in sync
    _Atomic uint32_t followers_lagging; // Number of followers behind
    _Atomic uint64_t max_follower_lag;  // Max log entries any follower is behind
    
    // Commit Progress
    _Atomic uint64_t commit_lag;        // Entries between last_applied and commit_index
    _Atomic uint64_t commit_rate;       // Commits per second (sampled)
    
    // Performance
    _Atomic uint64_t append_entries_rtt_us; // Average RPC latency (microseconds)
    _Atomic uint64_t log_replication_latency_ms; // Time from submit to commit
    
    // Errors & Failures
    _Atomic uint32_t rpc_failures;      // RPC timeouts/errors
    _Atomic uint32_t log_conflicts;     // AppendEntries rejections
    _Atomic uint32_t state_transitions; // Follower↔Candidate↔Leader changes
    
} __attribute__((aligned(128))) raft_operational_metrics_t;

// ============================================================================
// METRIC UPDATE API (Called from Raft state machine)
// ============================================================================

/**
 * Update leadership status
 * Called when transitioning to/from leader
 */
static inline void raft_metrics_set_leader(raft_operational_metrics_t *metrics, int is_leader) {
    atomic_store_explicit(&metrics->is_leader, is_leader, memory_order_relaxed);
    if (is_leader) {
        atomic_store_explicit(&metrics->leader_election_ts, time_now_ms(), memory_order_relaxed);
    }
}

/**
 * Record election event
 */
static inline void raft_metrics_record_election(raft_operational_metrics_t *metrics) {
    atomic_fetch_add_explicit(&metrics->elections_total, 1, memory_order_relaxed);
}

/**
 * Update follower health (Leader-only)
 * Called after successful AppendEntries
 */
static inline void raft_metrics_update_follower_health(raft_operational_metrics_t *metrics,
                                                       uint32_t healthy,
                                                       uint32_t lagging,
                                                       uint64_t max_lag) {
    atomic_store_explicit(&metrics->followers_healthy, healthy, memory_order_relaxed);
    atomic_store_explicit(&metrics->followers_lagging, lagging, memory_order_relaxed);
    atomic_store_explicit(&metrics->max_follower_lag, max_lag, memory_order_relaxed);
}

/**
 * Update commit lag
 * Called when commit_index advances
 */
static inline void raft_metrics_update_commit_lag(raft_operational_metrics_t *metrics,
                                                  uint64_t commit_index,
                                                  uint64_t last_applied) {
    uint64_t lag = (commit_index > last_applied) ? (commit_index - last_applied) : 0;
    atomic_store_explicit(&metrics->commit_lag, lag, memory_order_relaxed);
}

/**
 * Record RPC failure
 */
static inline void raft_metrics_record_rpc_failure(raft_operational_metrics_t *metrics) {
    atomic_fetch_add_explicit(&metrics->rpc_failures, 1, memory_order_relaxed);
}

/**
 * Record log conflict (AppendEntries rejection)
 */
static inline void raft_metrics_record_log_conflict(raft_operational_metrics_t *metrics) {
    atomic_fetch_add_explicit(&metrics->log_conflicts, 1, memory_order_relaxed);
}

// ============================================================================
// READ API (Lock-Free, for Prometheus Scrape)
// ============================================================================

/**
 * Check if this node is leader
 */
static inline int raft_metrics_is_leader(const raft_operational_metrics_t *metrics) {
    return atomic_load_explicit(&metrics->is_leader, memory_order_relaxed);
}

/**
 * Get commit lag (for alerting)
 */
static inline uint64_t raft_metrics_get_commit_lag(const raft_operational_metrics_t *metrics) {
    return atomic_load_explicit(&metrics->commit_lag, memory_order_relaxed);
}

/**
 * Get follower health status
 */
typedef struct {
    uint32_t followers_healthy;
    uint32_t followers_lagging;
    uint64_t max_follower_lag;
} raft_follower_health_t;

static inline void raft_metrics_get_follower_health(const raft_operational_metrics_t *metrics,
                                                    raft_follower_health_t *out) {
    out->followers_healthy = atomic_load_explicit(&metrics->followers_healthy, memory_order_relaxed);
    out->followers_lagging = atomic_load_explicit(&metrics->followers_lagging, memory_order_relaxed);
    out->max_follower_lag = atomic_load_explicit(&metrics->max_follower_lag, memory_order_relaxed);
}

// ============================================================================
// INTEGRATION WITH RAFT_STATE_T
// ============================================================================

// Add this field to raft_state_t (in raft_state.h):
// raft_operational_metrics_t op_metrics;

// Then update raft_state.c to call metric update functions:
// - In become_leader(): raft_metrics_set_leader(&state->op_metrics, 1)
// - In become_follower(): raft_metrics_set_leader(&state->op_metrics, 0)
// - In start_election(): raft_metrics_record_election(&state->op_metrics)
// - After AppendEntries RPC: raft_metrics_update_follower_health(...)
// - When commit advances: raft_metrics_update_commit_lag(...)

#endif // ROOLE_RAFT_OPERATIONAL_METRICS_H
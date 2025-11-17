// src/node/node_metrics.c
// REFACTORED: Direct atomic reads, no callbacks, clear separation

#define _POSIX_C_SOURCE 200809L

#include "roole/node/node_state.h"
#include "roole/raft/raft_datastore.h"
#include "roole/raft/raft_operational_metrics.h"
#include "roole/config/config.h"
#include "roole/core/common.h"
#include "roole/metrics/metrics.h"
#include <stdio.h>
#include <string.h>

// ============================================================================
// METRICS INITIALIZATION (No Callbacks!)
// ============================================================================

int node_metrics_init(node_state_t *state, const char *metrics_addr) {
    if (!state) return RESULT_ERR_INVALID;
    
    if (!metrics_addr || strlen(metrics_addr) == 0) {
        LOG_INFO("Metrics disabled: no metrics_addr configured");
        state->metrics_registry = NULL;
        state->metrics_server = NULL;
        return RESULT_OK;
    }
    
    // Parse metrics address
    char metrics_ip[16];
    uint16_t metrics_port;
    config_parse_address(metrics_addr, metrics_ip, &metrics_port);
    
    if (metrics_port == 0) {
        LOG_WARN("Metrics disabled: invalid port in config");
        state->metrics_registry = NULL;
        state->metrics_server = NULL;
        return RESULT_OK;
    }
    
    LOG_INFO("Initializing metrics system on %s:%u...", metrics_ip, metrics_port);
    
    // Create registry
    state->metrics_registry = metrics_registry_init();
    if (!state->metrics_registry) {
        LOG_WARN("Failed to initialize metrics registry");
        return RESULT_ERR_NOMEM;
    }
    
    // Get identity
    const node_identity_t *id = node_state_get_identity(state);
    const node_capabilities_t *caps = node_state_get_capabilities(state);
    
    // Build standard labels
    char node_id_str[32];
    snprintf(node_id_str, sizeof(node_id_str), "%u", id->node_id);
    
    const char *node_type_label = caps->has_ingress ? "router" : "worker";
    
    metric_label_t labels[3];
    safe_strncpy(labels[0].name, "cluster_name", MAX_LABEL_NAME_LEN);
    safe_strncpy(labels[0].value, id->cluster_name, MAX_LABEL_VALUE_LEN);
    safe_strncpy(labels[1].name, "node_id", MAX_LABEL_NAME_LEN);
    safe_strncpy(labels[1].value, node_id_str, MAX_LABEL_VALUE_LEN);
    safe_strncpy(labels[2].name, "node_type", MAX_LABEL_NAME_LEN);
    safe_strncpy(labels[2].value, node_type_label, MAX_LABEL_VALUE_LEN);
    
    // ========================================================================
    // DATASTORE METRICS (Atomic Counters - Zero Overhead)
    // ========================================================================
    
    state->metric_datastore_size = metrics_get_or_create_gauge(
        state->metrics_registry,
        "datastore_records",
        "Number of records in the Raft-backed datastore",
        3, labels
    );
    
    state->metric_datastore_bytes = metrics_get_or_create_gauge(
        state->metrics_registry,
        "datastore_bytes_total",
        "Total bytes stored in datastore values",
        3, labels
    );
    
    state->metric_datastore_sets = metrics_get_or_create_counter(
        state->metrics_registry,
        "datastore_sets_total",
        "Total number of SET operations",
        3, labels
    );
    
    state->metric_datastore_gets = metrics_get_or_create_counter(
        state->metrics_registry,
        "datastore_gets_total",
        "Total number of GET operations",
        3, labels
    );
    
    state->metric_datastore_unsets = metrics_get_or_create_counter(
        state->metrics_registry,
        "datastore_unsets_total",
        "Total number of UNSET operations",
        3, labels
    );

    // ========================================================================
    // RAFT OPERATIONAL METRICS (Actionable, Not Internal State!)
    // ========================================================================
    
    state->metric_raft_commit_lag = metrics_get_or_create_gauge(
        state->metrics_registry,
        "raft_commit_lag_entries",
        "Number of log entries between last_applied and commit_index",
        3, labels
    );
    
    state->metric_raft_followers_healthy = metrics_get_or_create_gauge(
        state->metrics_registry,
        "raft_followers_healthy",
        "Number of followers in sync with leader (leader-only)",
        3, labels
    );
    
    state->metric_raft_followers_lagging = metrics_get_or_create_gauge(
        state->metrics_registry,
        "raft_followers_lagging",
        "Number of followers behind leader (leader-only)",
        3, labels
    );
    
    state->metric_raft_elections_total = metrics_get_or_create_counter(
        state->metrics_registry,
        "raft_elections_total",
        "Total number of leader elections started",
        3, labels
    );
    
    // ========================================================================
    // CLUSTER METRICS (Gossip Layer)
    // ========================================================================
    
    state->metric_cluster_members_total = metrics_get_or_create_gauge(
        state->metrics_registry,
        "cluster_members_total",
        "Total number of cluster members known via gossip",
        3, labels
    );
    
    state->metric_cluster_members_active = metrics_get_or_create_gauge(
        state->metrics_registry,
        "cluster_members_active",
        "Number of active cluster members (SWIM ALIVE)",
        3, labels
    );
    
    // ========================================================================
    // SYSTEM METRICS
    // ========================================================================
    
    state->metric_uptime_seconds = metrics_get_or_create_gauge(
        state->metrics_registry,
        "uptime_seconds",
        "Node uptime in seconds",
        3, labels
    );
    
    // ========================================================================
    // HISTOGRAM METRICS (Performance Tracking)
    // ========================================================================
    
    state->histogram_raft_commit_latency = metrics_get_or_create_histogram(
        state->metrics_registry,
        "raft_commit_latency_ms",
        "Histogram of Raft commit latency (submit to commit)",
        HISTOGRAM_BUCKETS_LATENCY_MS,
        3, labels
    );
    
    LOG_INFO("✓ Metrics created (direct atomic reads, zero callbacks)");
    
    // ========================================================================
    // Start HTTP server
    // ========================================================================
    
    state->metrics_server = metrics_server_start(
        state->metrics_registry,
        metrics_ip,
        metrics_port
    );
    
    if (!state->metrics_server) {
        LOG_ERROR("Failed to start metrics HTTP server on %s:%u", 
                 metrics_ip, metrics_port);
        LOG_WARN("Continuing without metrics endpoint");
        return RESULT_ERR_NETWORK;
    }
    
    LOG_INFO("✓ Metrics HTTP server started on http://%s:%u/metrics", 
             metrics_ip, metrics_port);
    
    return RESULT_OK;
}

// ============================================================================
// PERIODIC METRICS UPDATE (Direct Atomic Reads)
// ============================================================================
void node_metrics_update_cluster(node_state_t *state) {
    if (!state) return;
    
    cluster_view_t *view = node_state_get_cluster_view(state);
    if (!view || !view->members) return;
    
    pthread_rwlock_rdlock(&view->lock);
    
    size_t total = view->count;
    size_t active = 0;
    size_t suspect = 0;
    size_t dead = 0;
    
    for (size_t i = 0; i < total; i++) {
        cluster_member_t *member = &view->members[i];
        
        switch (member->status) {
            case NODE_STATUS_ALIVE:
                active++;
                break;
            case NODE_STATUS_SUSPECT:
                suspect++;
                break;
            case NODE_STATUS_DEAD:
                dead++;
                break;
            default:
                break;
        }
    }
    
    pthread_rwlock_unlock(&view->lock);
    
    // Update metrics
    if (state->metric_cluster_members_total) {
        metrics_gauge_set(state->metric_cluster_members_total, (double)total);
    }
    if (state->metric_cluster_members_active) {
        metrics_gauge_set(state->metric_cluster_members_active, (double)active);
    }
}

void node_metrics_update_periodic(node_state_t *state) {
    if (!state || !state->metrics_registry) return;
    
    // ========================================================================
    // 1. UPTIME (Simple Counter)
    // ========================================================================
    if (state->metric_uptime_seconds) {
        uint64_t uptime_seconds = (time_now_ms() - state->start_time_ms) / 1000;
        metrics_gauge_set(state->metric_uptime_seconds, (double)uptime_seconds);
    }
    
    // ========================================================================
    // 2. DATASTORE METRICS (Direct Atomic Reads - O(1), Lock-Free)
    // ========================================================================
    if (state->raft_datastore) {
        // ✅ ZERO overhead: Read directly from atomic counters
        uint64_t record_count = raft_datastore_get_record_count(state->raft_datastore);
        uint64_t total_bytes = raft_datastore_get_total_bytes(state->raft_datastore);
        
        if (state->metric_datastore_size) {
            metrics_gauge_set(state->metric_datastore_size, (double)record_count);
        }
        
        if (state->metric_datastore_bytes) {
            metrics_gauge_set(state->metric_datastore_bytes, (double)total_bytes);
        }
        
        // Operation counters (also atomic)
        raft_datastore_op_stats_t op_stats;
        raft_datastore_get_op_stats(state->raft_datastore, &op_stats);
        
        if (state->metric_datastore_sets) {
            metrics_gauge_set(state->metric_datastore_sets, (double)op_stats.sets_completed);
        }
        if (state->metric_datastore_gets) {
            metrics_gauge_set(state->metric_datastore_gets, (double)op_stats.gets_completed);
        }
        if (state->metric_datastore_unsets) {
            metrics_gauge_set(state->metric_datastore_unsets, (double)op_stats.deletes_completed);
        }
    }
    
    // ========================================================================
    // 3. RAFT OPERATIONAL METRICS (Direct Atomic Reads)
    // ========================================================================
    if (state->raft_state && state->raft_state->op_metrics) {
        raft_operational_metrics_t *op = state->raft_state->op_metrics;
        
        // Commit lag (actionable - alert if too high)
        if (state->metric_raft_commit_lag) {
            uint64_t lag = raft_metrics_get_commit_lag(op);
            metrics_gauge_set(state->metric_raft_commit_lag, (double)lag);
        }
        
        // Follower health (leader-only, actionable)
        raft_follower_health_t health;
        raft_metrics_get_follower_health(op, &health);
        
        if (state->metric_raft_followers_healthy) {
            metrics_gauge_set(state->metric_raft_followers_healthy, 
                            (double)health.followers_healthy);
        }
        if (state->metric_raft_followers_lagging) {
            metrics_gauge_set(state->metric_raft_followers_lagging,
                            (double)health.followers_lagging);
        }
        
        // Election count (cumulative counter)
        if (state->metric_raft_elections_total) {
            uint32_t elections = atomic_load_explicit(&op->elections_total, 
                                                     memory_order_relaxed);
            metrics_gauge_set(state->metric_raft_elections_total, (double)elections);
        }
    } else {
        LOG_DEBUG("Raft operational metrics not available (op_metrics=%p)",
                  (void*)(state->raft_state ? state->raft_state->op_metrics : NULL));
    }
    
    // ========================================================================
    // 4. CLUSTER METRICS (Gossip Layer)
    // ========================================================================
    node_metrics_update_cluster(state);
    
    LOG_DEBUG("Metrics updated: records=%lu bytes=%lu leader=%d",
              raft_datastore_get_record_count(state->raft_datastore),
              raft_datastore_get_total_bytes(state->raft_datastore),
              state->raft_state ? raft_metrics_is_leader(state->raft_state->op_metrics) : 0);
}

// ============================================================================
// CLUSTER METRICS UPDATE (Unchanged - Gossip Layer)
// ============================================================================



void node_metrics_shutdown(node_state_t *state) {
    if (!state) return;
    
    if (state->metrics_server) {
        metrics_server_shutdown(state->metrics_server);
        state->metrics_server = NULL;
    }
    
    if (state->metrics_registry) {
        metrics_registry_destroy(state->metrics_registry);
        state->metrics_registry = NULL;
    }
    
    LOG_INFO("Metrics system shutdown complete");
}
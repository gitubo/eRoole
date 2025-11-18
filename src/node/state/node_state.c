// src/node/state/node_state.c
// REFACTORED: Raft-first architecture (removed old datastore)

#define _POSIX_C_SOURCE 200809L

#include "roole/node/node_state.h"
#include "roole/node/node_capabilities.h"
#include "roole/node/node_metrics.h"
#include "roole/raft/raft_rpc.h"
#include "roole/config/config.h"
#include "roole/core/service_registry.h"
#include "roole/core/common.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// ============================================================================
// GOSSIP → RAFT INTEGRATION CALLBACKS
// ============================================================================

/**
 * Gossip membership event callback
 * Bridges SWIM gossip events to Raft peer management
 * 
 * CRITICAL: During bootstrap, we DON'T remove Raft peers on gossip failures!
 * This prevents premature peer removal before Raft starts.
 */
static void on_gossip_membership_event(node_id_t node_id,
                                       node_type_t type,
                                       const char *ip,
                                       uint16_t data_port,
                                       const char *event_type,
                                       void *user_data) {
    node_state_t *state = (node_state_t*)user_data;
    
    if (!state || !state->raft_state) {
        LOG_WARN("Cannot process gossip event: Raft not initialized");
        return;
    }
    
    // Skip self
    if (node_id == state->identity.node_id) {
        return;
    }
    
    LOG_INFO("Gossip event: node=%u type=%s event=%s", 
             node_id, event_type, 
             type == NODE_TYPE_ROUTER ? "ROUTER" : "WORKER");
    
    if (strcmp(event_type, MEMBER_EVENT_JOIN) == 0) {
        // New peer joined → add to Raft cluster
        LOG_INFO("Adding peer %u to Raft cluster (%s:%u)", 
                 node_id, ip, data_port);
        if (data_port == 0) {
            LOG_ERROR("Cannot add peer %u: data_port is 0!", node_id);
            return;  
        }
        if (raft_add_peer(state->raft_state, node_id, ip, data_port) == 0) {
            LOG_INFO("✓ Peer %u added to Raft cluster", node_id);
            
            // Update metrics
            if (state->metric_cluster_members_active) {
                metrics_gauge_inc(state->metric_cluster_members_active);
            }
        } else {
            LOG_ERROR("✗ Failed to add peer %u to Raft", node_id);
        }
        
    } else if (strcmp(event_type, MEMBER_EVENT_FAILED) == 0 ||
               strcmp(event_type, MEMBER_EVENT_LEAVE) == 0) {
        
        // ✅ FIX: Check if we're in bootstrap grace period
        uint64_t now = time_now_ms();
        uint64_t time_since_bootstrap = now - state->bootstrap_complete_time_ms;
        
        if (state->bootstrap_complete_time_ms > 0 && 
            time_since_bootstrap < BOOTSTRAP_GRACE_PERIOD_MS) {
            LOG_WARN("Ignoring peer failure during bootstrap grace period");
            return;  // Don't remove Raft peer yet!
        }
        
        // After grace period, proceed with normal removal
        LOG_INFO("Removing peer %u from Raft cluster", node_id);
        
        if (raft_remove_peer(state->raft_state, node_id) == 0) {
            LOG_INFO("✓ Peer %u removed from Raft cluster", node_id);
            
            // Update metrics
            if (state->metric_cluster_members_active) {
                metrics_gauge_dec(state->metric_cluster_members_active);
            }
        } else {
            LOG_WARN("Peer %u not found in Raft cluster", node_id);
        }
        
    } else if (strcmp(event_type, MEMBER_EVENT_UPDATE) == 0) {
        // Peer updated (e.g., recovered from SUSPECT)
        LOG_DEBUG("Peer %u status updated", node_id);
        // No action needed for Raft - it manages its own health
    }
}

// ============================================================================
// HELPER: Parse address string "ip:port"
// ============================================================================

static void parse_addr_port(const char *addr_str, char *ip, uint16_t *port) {
    if (!addr_str || !ip || !port) return;
    
    const char *colon = strchr(addr_str, ':');
    if (colon) {
        size_t ip_len = colon - addr_str;
        if (ip_len >= MAX_IP_LEN) ip_len = MAX_IP_LEN - 1;
        
        strncpy(ip, addr_str, ip_len);
        ip[ip_len] = '\0';
        
        *port = (uint16_t)atoi(colon + 1);
    } else {
        safe_strncpy(ip, addr_str, MAX_IP_LEN);
        *port = 0;
    }
}

// ============================================================================
// BACKGROUND THREADS
// ============================================================================

static void* cleanup_thread_fn(void *arg) {
    node_state_t *state = (node_state_t*)arg;
    
    logger_push_component("cleanup");
    LOG_INFO("Cleanup thread started");
    
    while (!state->shutdown_flag) {
        sleep(60);  // Run every 60 seconds
        
        if (state->shutdown_flag) break;
        
        // Raft handles its own log compaction
        LOG_DEBUG("Cleanup cycle: Raft managing its own state");
    }
    
    LOG_INFO("Cleanup thread stopped");
    logger_pop_component();
    
    return NULL;
}

static void* metrics_update_thread_fn(void *arg) {
    node_state_t *state = (node_state_t*)arg;
    
    logger_push_component("metrics");
    LOG_INFO("Metrics update thread started");
    
    while (!state->shutdown_flag) {
        sleep(10);  // Update every 10 seconds
        
        if (state->shutdown_flag) break;
        
        // Update periodic metrics
        node_metrics_update_periodic(state);
    }
    
    LOG_INFO("Metrics update thread stopped");
    logger_pop_component();
    
    return NULL;
}

// ============================================================================
// PUBLIC API: NODE LIFECYCLE
// ============================================================================

result_t node_state_init(node_state_t **out_state, const roole_config_t *config) {
    if (!out_state || !config) {
        return RESULT_ERROR(RESULT_ERR_INVALID, "Invalid parameters");
    }
    
    LOG_INFO("Initializing node state (Raft-first architecture)");
    LOG_INFO("  Node ID: %u", config->node_id);
    LOG_INFO("  Type: %s", config->node_type == NODE_TYPE_ROUTER ? "ROUTER" : "WORKER");
    
    // Allocate state structure
    node_state_t *state = (node_state_t*)safe_calloc(1, sizeof(node_state_t));
    if (!state) {
        return RESULT_ERROR(RESULT_ERR_NOMEM, "Failed to allocate node state");
    }
    
    state->start_time_ms = time_now_ms();
    state->bootstrap_complete_time_ms = 0;
    state->shutdown_flag = 0;
    
    // ========================================================================
    // 1. Initialize Node Identity
    // ========================================================================
    
    state->identity.node_id = config->node_id;
    state->identity.node_type = config->node_type;
    safe_strncpy(state->identity.cluster_name, config->cluster_name, 
                 sizeof(state->identity.cluster_name));
    
    // Parse addresses
    parse_addr_port(config->ports.gossip_addr, state->identity.bind_addr,
                    &state->identity.gossip_port);
    parse_addr_port(config->ports.data_addr, state->identity.bind_addr,
                    &state->identity.data_port);
    parse_addr_port(config->ports.ingress_addr, state->identity.bind_addr,
                    &state->identity.ingress_port);
    parse_addr_port(config->ports.metrics_addr, state->identity.bind_addr,
                    &state->identity.metrics_port);
    
    // Detect capabilities
    node_detect_capabilities(config, &state->capabilities, &state->identity);
    node_print_capabilities(&state->capabilities, &state->identity);
    
    // ========================================================================
    // 2. Initialize Cluster View (Shared by Gossip and Raft)
    // ========================================================================
    
    state->cluster_view = (cluster_view_t*)safe_calloc(1, sizeof(cluster_view_t));
    if (!state->cluster_view) {
        safe_free(state);
        return RESULT_ERROR(RESULT_ERR_NOMEM, "Failed to allocate cluster view");
    }
    
    if (cluster_view_init(state->cluster_view, MAX_CLUSTER_NODES) != RESULT_OK) {
        safe_free(state->cluster_view);
        safe_free(state);
        return RESULT_ERROR(RESULT_ERR_INVALID, "Failed to initialize cluster view");
    }
    
    LOG_INFO("✓ Cluster view initialized (capacity: %d nodes)", MAX_CLUSTER_NODES);
    
    // ========================================================================
    // 3. Initialize Raft Consensus (BEFORE Membership/Gossip)
    // ========================================================================
    
    LOG_INFO("Initializing Raft consensus state machine...");
    
    // Create Raft state machine
    raft_config_t raft_config = raft_default_config();
    
    // Raft callbacks will be set after datastore creation
    raft_callbacks_t raft_callbacks = {
        .on_apply = NULL,  // Set after datastore init
        .on_snapshot_create = NULL,
        .on_snapshot_restore = NULL,
        .user_data = NULL
    };
    
    state->raft_state = raft_state_create(
        config->node_id,
        state->cluster_view,
        &raft_config,
        &raft_callbacks
    );
    
    if (!state->raft_state) {
        LOG_ERROR("Failed to create Raft state machine");
        cluster_view_destroy(state->cluster_view);
        safe_free(state->cluster_view);
        safe_free(state);
        return RESULT_ERROR(RESULT_ERR_INVALID, "Raft state creation failed");
    }
    
    LOG_INFO("✓ Raft state machine created");
    
    // ========================================================================
    // 4. Initialize Raft-Backed Datastore (PRIMARY STORAGE)
    // ========================================================================
    
    LOG_INFO("Creating Raft-backed strongly consistent datastore...");
    
    state->raft_datastore = raft_datastore_create(state->raft_state, RAFT_KV_MAX_RECORDS);
    
    if (!state->raft_datastore) {
        LOG_ERROR("Failed to create Raft datastore");
        raft_state_destroy(state->raft_state);
        cluster_view_destroy(state->cluster_view);
        safe_free(state->cluster_view);
        safe_free(state);
        return RESULT_ERROR(RESULT_ERR_NOMEM, "Raft datastore creation failed");
    }
    
    // Update Raft callbacks to point to datastore
    raft_callbacks.on_apply = raft_datastore_apply;
    raft_callbacks.on_snapshot_create = raft_datastore_snapshot;
    raft_callbacks.on_snapshot_restore = raft_datastore_restore;
    raft_callbacks.user_data = state->raft_datastore;
    
    // TODO: Add API to update callbacks in raft_state.c
    // For now, callbacks are set during creation
    
    LOG_INFO("✓ Raft datastore initialized (capacity: %d records)", RAFT_KV_MAX_RECORDS);
    LOG_INFO("  Consistency: STRONG (linearizable reads/writes)");
    LOG_INFO("  Consensus: Raft");
    
    // ========================================================================
    // 5. Initialize Membership/Gossip (AFTER Raft, for peer discovery)
    // ========================================================================
    
    LOG_INFO("Initializing membership (SWIM gossip for peer discovery)...");
    
    if (membership_init(&state->membership,
                       config->node_id,
                       config->node_type,
                       state->identity.bind_addr,
                       state->identity.gossip_port,
                       state->identity.data_port,
                       state->cluster_view) != RESULT_OK) {
        raft_datastore_destroy(state->raft_datastore);
        raft_state_destroy(state->raft_state);
        cluster_view_destroy(state->cluster_view);
        safe_free(state->cluster_view);
        safe_free(state);
        return RESULT_ERROR(RESULT_ERR_INVALID, "Membership initialization failed");
    }
    LOG_INFO("Initializing membership with data_port=%u", state->identity.data_port);
    
    // Set gossip callback to bridge to Raft
    membership_set_callback(state->membership, 
                           on_gossip_membership_event, 
                           state);
    
    LOG_INFO("✓ Membership initialized with Gossip→Raft bridge");
    
    // ========================================================================
    // 6. Initialize Peer Pool (For RPC connection management)
    // ========================================================================
    
    state->peer_pool = (peer_pool_t*)safe_calloc(1, sizeof(peer_pool_t));
    if (!state->peer_pool) {
        membership_shutdown(state->membership);
        raft_datastore_destroy(state->raft_datastore);
        raft_state_destroy(state->raft_state);
        cluster_view_destroy(state->cluster_view);
        safe_free(state->cluster_view);
        safe_free(state);
        return RESULT_ERROR(RESULT_ERR_NOMEM, "Failed to allocate peer pool");
    }
    
    if (peer_pool_init(state->peer_pool, MAX_PEERS) != RESULT_OK) {
        safe_free(state->peer_pool);
        membership_shutdown(state->membership);
        raft_datastore_destroy(state->raft_datastore);
        raft_state_destroy(state->raft_state);
        cluster_view_destroy(state->cluster_view);
        safe_free(state->cluster_view);
        safe_free(state);
        return RESULT_ERROR(RESULT_ERR_INVALID, "Peer pool initialization failed");
    }
    
    // ========================================================================
    // 7. Initialize Event Bus
    // ========================================================================
    
    state->event_bus = event_bus_create();
    if (!state->event_bus) {
        peer_pool_destroy(state->peer_pool);
        safe_free(state->peer_pool);
        membership_shutdown(state->membership);
        raft_datastore_destroy(state->raft_datastore);
        raft_state_destroy(state->raft_state);
        cluster_view_destroy(state->cluster_view);
        safe_free(state->cluster_view);
        safe_free(state);
        return RESULT_ERROR(RESULT_ERR_NOMEM, "Failed to create event bus");
    }
    
    // ========================================================================
    // 8. Initialize Metrics
    // ========================================================================
    
    if (config->ports.metrics_addr[0] != '\0') {
        if (node_metrics_init(state, config->ports.metrics_addr) != RESULT_OK) {
            LOG_WARN("Failed to initialize metrics (continuing without metrics)");
        }
    } else {
        LOG_INFO("Metrics disabled (no metrics address configured)");
    }
    
    // ========================================================================
    // 9. Register in Service Registry
    // ========================================================================
    
    service_registry_t *registry = service_registry_global();
    if (registry) {
        service_registry_register(registry, SERVICE_TYPE_NODE_STATE, 
                                 "main", state);
        service_registry_register(registry, SERVICE_TYPE_CLUSTER_VIEW,
                                 "main", state->cluster_view);
        service_registry_register(registry, SERVICE_TYPE_EVENT_BUS,
                                 "main", state->event_bus);
        if (state->metrics_registry) {
            service_registry_register(registry, SERVICE_TYPE_METRICS,
                                     "main", state->metrics_registry);
        }
    }
    
    *out_state = state;
    
    LOG_INFO("========================================");
    LOG_INFO("Node State Initialized (Raft-First)");
    LOG_INFO("  Storage: Raft-backed KV store");
    LOG_INFO("  Consistency: STRONG (linearizable)");
    LOG_INFO("  Membership: SWIM gossip");
    LOG_INFO("  Consensus: Raft");
    LOG_INFO("========================================");
    
    return RESULT_SUCCESS();
}

result_t node_state_start(node_state_t *state) {
    if (!state) {
        return RESULT_ERROR(RESULT_ERR_INVALID, "NULL state");
    }
    
    LOG_INFO("Starting node services...");
    
    // ========================================================================
    // CRITICAL: Raft start timing depends on bootstrap mode
    // - SEED nodes (no router config): Start Raft immediately
    // - JOINING nodes (have router config): Start Raft AFTER gossip discovery
    // ========================================================================
    
    // Check if this is a seed node (no routers configured)
    // For now, we'll start Raft immediately for all nodes
    // The bootstrap phase will handle delayed start for workers
    
    // Start background threads (NOT Raft yet)
    LOG_INFO("[1/2] Starting background threads...");
    
    if (pthread_create(&state->cleanup_thread, NULL, cleanup_thread_fn, state) != 0) {
        LOG_ERROR("Failed to create cleanup thread");
        return RESULT_ERROR(RESULT_ERR_INVALID, "Cleanup thread creation failed");
    }
    
    if (state->metrics_registry) {
        if (pthread_create(&state->metrics_update_thread, NULL, 
                          metrics_update_thread_fn, state) != 0) {
            LOG_ERROR("Failed to create metrics update thread");
            state->shutdown_flag = 1;
            pthread_join(state->cleanup_thread, NULL);
            return RESULT_ERROR(RESULT_ERR_INVALID, "Metrics thread creation failed");
        }
    }
    
    LOG_INFO("✓ Background threads started");
    
    return RESULT_SUCCESS();
}

// ========================================================================
// NEW: Separate function to start Raft (called after bootstrap)
// ========================================================================

result_t node_state_start_raft(node_state_t *state) {
    if (!state || !state->raft_state) {
        return RESULT_ERROR(RESULT_ERR_INVALID, "Invalid state or Raft not initialized");
    }
    
    LOG_INFO("Starting Raft consensus state machine...");
    
    if (raft_state_start(state->raft_state) != 0) {
        LOG_ERROR("Failed to start Raft state machine");
        return RESULT_ERROR(RESULT_ERR_INVALID, "Raft start failed");
    }
    
    LOG_INFO("✓ Raft consensus started");
    
    return RESULT_SUCCESS();
}

// ========================================================================
// REFACTORED: Bootstrap with delayed Raft start
// ========================================================================

result_t node_state_bootstrap(node_state_t *state, const roole_config_t *config) {
    if (!state || !config) {
        return RESULT_ERROR(RESULT_ERR_INVALID, "Invalid parameters");
    }
    
    // ========================================================================
    // CASE 1: SEED NODE (No routers configured)
    // ========================================================================
    if (config->router_count == 0) {
        LOG_INFO("Operating as SEED node - starting Raft immediately");
        
        // Start Raft consensus (will become leader)
        result_t raft_result = node_state_start_raft(state);
        if (result_is_error(&raft_result)) {
            return raft_result;
        }
        
        // Mark bootstrap complete (grace period starts now)
        state->bootstrap_complete_time_ms = time_now_ms();
        LOG_INFO("Seed node ready (grace period active for %d seconds)",
                 BOOTSTRAP_GRACE_PERIOD_MS / 1000);
        return RESULT_SUCCESS();
    }
    
    // ========================================================================
    // CASE 2: JOINING NODE (Has router config)
    // ========================================================================
    LOG_INFO("Joining existing cluster via %zu seed router(s)...", config->router_count);
    LOG_INFO("⏳ Delaying Raft start until cluster discovery completes");
    
    // Try to join via gossip
    int joined = 0;
    for (size_t i = 0; i < config->router_count; i++) {
        char seed_ip[16];
        uint16_t seed_port;
        config_parse_address(config->routers[i], seed_ip, &seed_port);
        
        LOG_INFO("Attempting to join via seed: %s:%u", seed_ip, seed_port);
        
        if (membership_join(state->membership, seed_ip, seed_port) == RESULT_OK) {
            LOG_INFO("Successfully sent JOIN to seed %s:%u", seed_ip, seed_port);
            joined = 1;
            break;
        } else {
            LOG_WARN("Failed to join via seed %s:%u", seed_ip, seed_port);
        }
    }
    
    if (!joined) {
        return RESULT_ERROR(RESULT_ERR_NETWORK, 
                           "Failed to join cluster via any seed router");
    }
    
    // ========================================================================
    // Wait for cluster discovery via gossip
    // ========================================================================
    LOG_INFO("Waiting for cluster discovery...");
    
    const int max_wait_seconds = 10;
    const int check_interval_ms = 500;
    int total_wait_ms = 0;
    
    while (total_wait_ms < max_wait_seconds * 1000) {
        usleep(check_interval_ms * 1000);
        total_wait_ms += check_interval_ms;
        
        size_t member_count = state->cluster_view->count;
        
        // Wait until we discover at least one peer (besides self)
        if (member_count > 1) {
            LOG_INFO("✓ Cluster discovered: %zu members total", member_count);
            
            // Give Raft peer connections time to establish
            LOG_INFO("Waiting for Raft peer connections to establish...");
            sleep(2);
            
            break;
        }
        
        if (total_wait_ms % 2000 == 0) {
            LOG_DEBUG("Still waiting for cluster discovery... (%d/%d seconds)",
                     total_wait_ms / 1000, max_wait_seconds);
        }
    }
    
    size_t final_member_count = state->cluster_view->count;
    
    if (final_member_count <= 1) {
        LOG_WARN("Cluster discovery timeout - only found self");
        LOG_WARN("Starting Raft anyway (may become isolated leader)");
    } else {
        cluster_view_dump(state->cluster_view, "After Cluster Discovery");
    }
    
    // ========================================================================
    // NOW start Raft (after cluster is known)
    // ========================================================================
    LOG_INFO("Starting Raft consensus (joining existing cluster)...");
    
    LOG_INFO("✓ Raft started - will sync with existing leader");
    state->bootstrap_complete_time_ms = time_now_ms();
    LOG_INFO("Bootstrap complete - grace period active for %d seconds",
             BOOTSTRAP_GRACE_PERIOD_MS / 1000);

    // NOW start Raft (after grace period is active)
    result_t raft_result = node_state_start_raft(state);
    if (result_is_error(&raft_result)) {
        return raft_result;
    }

    return RESULT_SUCCESS();
}


void node_state_shutdown(node_state_t *state) {
    if (!state) return;
    
    LOG_INFO("Shutting down node...");
    
    // Signal shutdown
    state->shutdown_flag = 1;
    
    // Gracefully leave cluster (gossip)
    if (state->membership) {
        LOG_INFO("Leaving cluster gracefully (gossip)...");
        membership_leave(state->membership);
        sleep(1);  // Give time for LEAVE message to propagate
    }
    
    // Stop Raft (will step down if leader)
    if (state->raft_state) {
        LOG_INFO("Stopping Raft consensus...");
        raft_state_stop(state->raft_state);
    }
    
    // Stop cleanup thread
    if (state->cleanup_thread) {
        pthread_join(state->cleanup_thread, NULL);
    }
    
    // Stop metrics update thread
    if (state->metrics_update_thread) {
        pthread_join(state->metrics_update_thread, NULL);
    }

    LOG_INFO("Node shutdown complete");
}

void node_state_destroy(node_state_t *state) {
    if (!state) return;
    
    LOG_INFO("Destroying node state...");
    
    // Ensure shutdown was called
    if (!state->shutdown_flag) {
        node_state_shutdown(state);
    }
    
    // Destroy subsystems (reverse order of creation)
    node_metrics_shutdown(state);
    
    if (state->event_bus) {
        event_bus_destroy(state->event_bus);
        state->event_bus = NULL;
    }
    
    if (state->peer_pool) {
        peer_pool_destroy(state->peer_pool);
        safe_free(state->peer_pool);
        state->peer_pool = NULL;
    }
    
    if (state->membership) {
        membership_shutdown(state->membership);
        state->membership = NULL;
    }
    
    // Destroy Raft datastore BEFORE Raft state
    if (state->raft_datastore) {
        LOG_INFO("Destroying Raft datastore...");
        raft_datastore_destroy(state->raft_datastore);
        state->raft_datastore = NULL;
    }
    
    if (state->raft_state) {
        LOG_INFO("Destroying Raft state machine...");
        raft_state_destroy(state->raft_state);
        state->raft_state = NULL;
    }
    
    if (state->cluster_view) {
        cluster_view_destroy(state->cluster_view);
        safe_free(state->cluster_view);
        state->cluster_view = NULL;
    }
    
    safe_free(state);
    
    LOG_INFO("Node state destroyed");
}

// ============================================================================
// ACCESSOR FUNCTIONS (remove old datastore getter)
// ============================================================================

const node_identity_t* node_state_get_identity(const node_state_t *state) {
    return state ? &state->identity : NULL;
}

const node_capabilities_t* node_state_get_capabilities(const node_state_t *state) {
    return state ? &state->capabilities : NULL;
}

// REMOVED: Old eventually-consistent datastore
// datastore_t* node_state_get_datastore(node_state_t *state)

peer_pool_t* node_state_get_peer_pool(node_state_t *state) {
    return state ? state->peer_pool : NULL;
}

cluster_view_t* node_state_get_cluster_view(node_state_t *state) {
    return state ? state->cluster_view : NULL;
}

metrics_registry_t* node_state_get_metrics(node_state_t *state) {
    return state ? state->metrics_registry : NULL;
}

event_bus_t* node_state_get_event_bus(node_state_t *state) {
    return state ? state->event_bus : NULL;
}

void node_state_get_statistics(const node_state_t *state, node_statistics_t *stats) {
    if (!state || !stats) return;
    
    memset(stats, 0, sizeof(node_statistics_t));
    
    stats->uptime_ms = time_now_ms() - state->start_time_ms;
    stats->datastore_ops_total = state->datastore_ops_total;
    
    // Get stats from Raft datastore
    if (state->raft_datastore) {
        raft_datastore_stats_t ds_stats;
        raft_datastore_get_stats(state->raft_datastore, &ds_stats);
        stats->datastore_records = ds_stats.record_count;
        stats->datastore_bytes = ds_stats.total_bytes;
    }
    
    if (state->cluster_view) {
        stats->cluster_size = state->cluster_view->count;
    }
}
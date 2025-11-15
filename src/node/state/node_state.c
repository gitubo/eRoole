// src/node/state/node_state.c
// Simplified node state lifecycle - Pure datastore (no execution)

#define _POSIX_C_SOURCE 200809L

#include "roole/node/node_state.h"
#include "roole/node/node_capabilities.h"
#include "roole/node/node_metrics.h"
#include "roole/config/config.h"
#include "roole/core/service_registry.h"
#include "roole/core/common.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
// CLEANUP THREAD (Periodic maintenance)
// ============================================================================

static void* cleanup_thread_fn(void *arg) {
    node_state_t *state = (node_state_t*)arg;
    
    logger_push_component("cleanup");
    LOG_INFO("Cleanup thread started");
    
    while (!state->shutdown_flag) {
        sleep(60);  // Run every 60 seconds
        
        if (state->shutdown_flag) break;
        
        // Cleanup tombstones from datastore (after gossip propagation window)
        if (state->datastore) {
            // Simple maintenance - can be extended
            LOG_DEBUG("Cleanup cycle completed");
        }
    }
    
    LOG_INFO("Cleanup thread stopped");
    logger_pop_component();
    
    return NULL;
}

// ============================================================================
// METRICS UPDATE THREAD
// ============================================================================

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
    
    LOG_INFO("Initializing node state (node_id=%u, type=%d)",
             config->node_id, config->node_type);
    
    // Allocate state structure
    node_state_t *state = (node_state_t*)safe_calloc(1, sizeof(node_state_t));
    if (!state) {
        return RESULT_ERROR(RESULT_ERR_NOMEM, "Failed to allocate node state");
    }
    
    state->start_time_ms = time_now_ms();
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
    // 2. Initialize Datastore (Core Subsystem)
    // ========================================================================
    
    state->datastore = (datastore_t*)safe_calloc(1, sizeof(datastore_t));
    if (!state->datastore) {
        safe_free(state);
        return RESULT_ERROR(RESULT_ERR_NOMEM, "Failed to allocate datastore");
    }
    
    if (datastore_init(state->datastore, MAX_RECORDS) != RESULT_OK) {
        safe_free(state->datastore);
        safe_free(state);
        return RESULT_ERROR(RESULT_ERR_INVALID, "Failed to initialize datastore");
    }
    
    LOG_INFO("Datastore initialized (capacity: %d records)", MAX_RECORDS);
    
    // ========================================================================
    // 3. Initialize Peer Pool
    // ========================================================================
    
    state->peer_pool = (peer_pool_t*)safe_calloc(1, sizeof(peer_pool_t));
    if (!state->peer_pool) {
        datastore_destroy(state->datastore);
        safe_free(state->datastore);
        safe_free(state);
        return RESULT_ERROR(RESULT_ERR_NOMEM, "Failed to allocate peer pool");
    }
    
    if (peer_pool_init(state->peer_pool, MAX_PEERS) != RESULT_OK) {
        datastore_destroy(state->datastore);
        safe_free(state->datastore);
        safe_free(state->peer_pool);
        safe_free(state);
        return RESULT_ERROR(RESULT_ERR_INVALID, "Failed to initialize peer pool");
    }
    
    // ========================================================================
    // 4. Initialize Cluster View
    // ========================================================================
    
    state->cluster_view = (cluster_view_t*)safe_calloc(1, sizeof(cluster_view_t));
    if (!state->cluster_view) {
        peer_pool_destroy(state->peer_pool);
        safe_free(state->peer_pool);
        datastore_destroy(state->datastore);
        safe_free(state->datastore);
        safe_free(state);
        return RESULT_ERROR(RESULT_ERR_NOMEM, "Failed to allocate cluster view");
    }
    
    if (cluster_view_init(state->cluster_view, MAX_CLUSTER_NODES) != RESULT_OK) {
        peer_pool_destroy(state->peer_pool);
        safe_free(state->peer_pool);
        datastore_destroy(state->datastore);
        safe_free(state->datastore);
        safe_free(state->cluster_view);
        safe_free(state);
        return RESULT_ERROR(RESULT_ERR_INVALID, "Failed to initialize cluster view");
    }
    
    // ========================================================================
    // 5. Initialize Membership
    // ========================================================================
    
    if (membership_init(&state->membership,
                       config->node_id,
                       config->node_type,
                       state->identity.bind_addr,
                       state->identity.gossip_port,
                       state->identity.data_port,
                       state->cluster_view) != RESULT_OK) {
        cluster_view_destroy(state->cluster_view);
        safe_free(state->cluster_view);
        peer_pool_destroy(state->peer_pool);
        safe_free(state->peer_pool);
        datastore_destroy(state->datastore);
        safe_free(state->datastore);
        safe_free(state);
        return RESULT_ERROR(RESULT_ERR_INVALID, "Failed to initialize membership");
    }
    
    // ========================================================================
    // 6. Initialize Event Bus
    // ========================================================================
    
    state->event_bus = event_bus_create();
    if (!state->event_bus) {
        membership_shutdown(state->membership);
        cluster_view_destroy(state->cluster_view);
        safe_free(state->cluster_view);
        peer_pool_destroy(state->peer_pool);
        safe_free(state->peer_pool);
        datastore_destroy(state->datastore);
        safe_free(state->datastore);
        safe_free(state);
        return RESULT_ERROR(RESULT_ERR_NOMEM, "Failed to create event bus");
    }
    
    // ========================================================================
    // 7. Initialize Metrics
    // ========================================================================
    
    if (config->ports.metrics_addr[0] != '\0') {
        if (node_metrics_init(state, config->ports.metrics_addr) != RESULT_OK) {
            LOG_WARN("Failed to initialize metrics (continuing without metrics)");
        }
    } else {
        LOG_INFO("Metrics disabled (no metrics address configured)");
    }
    
    // ========================================================================
    // 8. Register in Service Registry
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
    
    LOG_INFO("Node state initialized successfully (pure datastore node)");
    return RESULT_SUCCESS();
}

result_t node_state_start(node_state_t *state) {
    if (!state) {
        return RESULT_ERROR(RESULT_ERR_INVALID, "NULL state");
    }
    
    LOG_INFO("Starting node services (no executor threads)...");
    
    // Start cleanup thread
    if (pthread_create(&state->cleanup_thread, NULL, cleanup_thread_fn, state) != 0) {
        LOG_ERROR("Failed to create cleanup thread");
        return RESULT_ERROR(RESULT_ERR_INVALID, "Failed to start cleanup thread");
    }
    
    // Start metrics update thread (if metrics enabled)
    if (state->metrics_registry) {
        if (pthread_create(&state->metrics_update_thread, NULL, 
                          metrics_update_thread_fn, state) != 0) {
            LOG_ERROR("Failed to create metrics update thread");
            state->shutdown_flag = 1;
            pthread_join(state->cleanup_thread, NULL);
            return RESULT_ERROR(RESULT_ERR_INVALID, "Failed to start metrics thread");
        }
    }
    
    LOG_INFO("Node services started successfully");
    return RESULT_SUCCESS();
}

result_t node_state_bootstrap(node_state_t *state, const roole_config_t *config) {
    if (!state || !config) {
        return RESULT_ERROR(RESULT_ERR_INVALID, "Invalid parameters");
    }
    
    if (config->router_count == 0) {
        LOG_INFO("No seed routers configured - operating as standalone/seed node");
        return RESULT_SUCCESS();
    }
    
    LOG_INFO("Joining cluster via %zu seed router(s)...", config->router_count);
    
    // Try each seed router
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
    
    // Wait for cluster view to populate
    LOG_INFO("Waiting for cluster view to populate...");
    sleep(5);  // Simple wait for gossip to propagate
    
    size_t member_count = state->cluster_view->count;
    LOG_INFO("Cluster membership discovered: %zu members", member_count);
    
    if (member_count > 1) {
        cluster_view_dump(state->cluster_view, "After Bootstrap");
        return RESULT_SUCCESS();
    }
    
    LOG_WARN("Only discovered self in cluster, continuing anyway");
    return RESULT_SUCCESS();
}

void node_state_shutdown(node_state_t *state) {
    if (!state) return;
    
    LOG_INFO("Shutting down node...");
    
    // Signal shutdown
    state->shutdown_flag = 1;
    
    // Gracefully leave cluster
    if (state->membership) {
        LOG_INFO("Leaving cluster gracefully...");
        membership_leave(state->membership);
        sleep(1);  // Give time for LEAVE message to propagate
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
    
    if (state->membership) {
        membership_shutdown(state->membership);
        state->membership = NULL;
    }
    
    if (state->cluster_view) {
        cluster_view_destroy(state->cluster_view);
        safe_free(state->cluster_view);
        state->cluster_view = NULL;
    }
    
    if (state->peer_pool) {
        peer_pool_destroy(state->peer_pool);
        safe_free(state->peer_pool);
        state->peer_pool = NULL;
    }
    
    if (state->datastore) {
        datastore_destroy(state->datastore);
        safe_free(state->datastore);
        state->datastore = NULL;
    }
    
    safe_free(state);
    
    LOG_INFO("Node state destroyed");
}

// ============================================================================
// ACCESSOR FUNCTIONS
// ============================================================================

const node_identity_t* node_state_get_identity(const node_state_t *state) {
    return state ? &state->identity : NULL;
}

const node_capabilities_t* node_state_get_capabilities(const node_state_t *state) {
    return state ? &state->capabilities : NULL;
}

datastore_t* node_state_get_datastore(node_state_t *state) {
    return state ? state->datastore : NULL;
}

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
    
    if (state->datastore) {
        stats->datastore_records = datastore_count(state->datastore);
        
        datastore_stats_t ds_stats;
        datastore_get_stats(state->datastore, &ds_stats);
        stats->datastore_bytes = ds_stats.total_value_bytes;
    }
    
    if (state->cluster_view) {
        stats->cluster_size = state->cluster_view->count;
    }
}
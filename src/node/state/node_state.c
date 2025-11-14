// src/node/state/node_state.c
// Node state lifecycle management - complete implementation

#define _POSIX_C_SOURCE 200809L

#include "roole/node/node_state.h"
#include "roole/node/node_capabilities.h"
#include "roole/node/node_executor.h"
#include "roole/node/node_metrics.h"
#include "roole/core/config.h"
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
        
        // Cleanup old completed executions
        if (state->exec_tracker) {
            size_t cleaned = execution_tracker_cleanup_completed(state->exec_tracker);
            if (cleaned > 0) {
                LOG_DEBUG("Cleaned up %zu completed executions", cleaned);
            }
        }
        
        // TODO: Add more periodic cleanup tasks:
        // - Stale peer connections
        // - Old metric samples
        // - Event bus queue overflow
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

result_t node_state_init(node_state_t **out_state, 
                         const roole_config_t *config,
                         size_t num_executor_threads) {
    if (!out_state || !config) {
        return RESULT_ERROR(RESULT_ERR_INVALID, "Invalid parameters");
    }
    
    if (num_executor_threads == 0) {
        num_executor_threads = 4;  // Default
    }
    
    LOG_INFO("Initializing node state (node_id=%u, type=%d, executors=%zu)",
             config->node_id, config->node_type, num_executor_threads);
    
    // Allocate state structure
    node_state_t *state = (node_state_t*)safe_calloc(1, sizeof(node_state_t));
    if (!state) {
        return RESULT_ERROR(RESULT_ERR_NOMEM, "Failed to allocate node state");
    }
    
    state->start_time_ms = time_now_ms();
    state->shutdown_flag = 0;
    state->num_executor_threads = num_executor_threads;
    
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
    // 2. Initialize Subsystems
    // ========================================================================
    
    // DAG Catalog
    state->dag_catalog = (dag_catalog_t*)safe_calloc(1, sizeof(dag_catalog_t));
    if (!state->dag_catalog) {
        safe_free(state);
        return RESULT_ERROR(RESULT_ERR_NOMEM, "Failed to allocate DAG catalog");
    }
    
    if (dag_catalog_init(state->dag_catalog, MAX_DAGS) != RESULT_OK) {
        safe_free(state->dag_catalog);
        safe_free(state);
        return RESULT_ERROR(RESULT_ERR_INVALID, "Failed to initialize DAG catalog");
    }
    
    // Peer Pool
    state->peer_pool = (peer_pool_t*)safe_calloc(1, sizeof(peer_pool_t));
    if (!state->peer_pool) {
        dag_catalog_destroy(state->dag_catalog);
        safe_free(state->dag_catalog);
        safe_free(state);
        return RESULT_ERROR(RESULT_ERR_NOMEM, "Failed to allocate peer pool");
    }
    
    if (peer_pool_init(state->peer_pool, MAX_PEERS) != RESULT_OK) {
        dag_catalog_destroy(state->dag_catalog);
        safe_free(state->dag_catalog);
        safe_free(state->peer_pool);
        safe_free(state);
        return RESULT_ERROR(RESULT_ERR_INVALID, "Failed to initialize peer pool");
    }
    
    // Execution Tracker
    state->exec_tracker = (execution_tracker_t*)safe_calloc(1, sizeof(execution_tracker_t));
    if (!state->exec_tracker) {
        peer_pool_destroy(state->peer_pool);
        safe_free(state->peer_pool);
        dag_catalog_destroy(state->dag_catalog);
        safe_free(state->dag_catalog);
        safe_free(state);
        return RESULT_ERROR(RESULT_ERR_NOMEM, "Failed to allocate exec tracker");
    }
    
    if (execution_tracker_init(state->exec_tracker, MAX_PENDING_EXECUTIONS) != RESULT_OK) {
        peer_pool_destroy(state->peer_pool);
        safe_free(state->peer_pool);
        dag_catalog_destroy(state->dag_catalog);
        safe_free(state->dag_catalog);
        safe_free(state->exec_tracker);
        safe_free(state);
        return RESULT_ERROR(RESULT_ERR_INVALID, "Failed to initialize exec tracker");
    }
    
    // Message Queue
    state->message_queue = (message_queue_t*)safe_calloc(1, sizeof(message_queue_t));
    if (!state->message_queue) {
        execution_tracker_destroy(state->exec_tracker);
        safe_free(state->exec_tracker);
        peer_pool_destroy(state->peer_pool);
        safe_free(state->peer_pool);
        dag_catalog_destroy(state->dag_catalog);
        safe_free(state->dag_catalog);
        safe_free(state);
        return RESULT_ERROR(RESULT_ERR_NOMEM, "Failed to allocate message queue");
    }
    
    if (message_queue_init(state->message_queue, MAX_NODE_QUEUE_SIZE) != RESULT_OK) {
        execution_tracker_destroy(state->exec_tracker);
        safe_free(state->exec_tracker);
        peer_pool_destroy(state->peer_pool);
        safe_free(state->peer_pool);
        dag_catalog_destroy(state->dag_catalog);
        safe_free(state->dag_catalog);
        safe_free(state->message_queue);
        safe_free(state);
        return RESULT_ERROR(RESULT_ERR_INVALID, "Failed to initialize message queue");
    }
    
    // ========================================================================
    // 3. Initialize Cluster View (Owned by Node)
    // ========================================================================
    
    state->cluster_view = (cluster_view_t*)safe_calloc(1, sizeof(cluster_view_t));
    if (!state->cluster_view) {
        // Cleanup already allocated
        message_queue_destroy(state->message_queue);
        safe_free(state->message_queue);
        execution_tracker_destroy(state->exec_tracker);
        safe_free(state->exec_tracker);
        peer_pool_destroy(state->peer_pool);
        safe_free(state->peer_pool);
        dag_catalog_destroy(state->dag_catalog);
        safe_free(state->dag_catalog);
        safe_free(state);
        return RESULT_ERROR(RESULT_ERR_NOMEM, "Failed to allocate cluster view");
    }
    
    if (cluster_view_init(state->cluster_view, MAX_CLUSTER_NODES) != RESULT_OK) {
        message_queue_destroy(state->message_queue);
        safe_free(state->message_queue);
        execution_tracker_destroy(state->exec_tracker);
        safe_free(state->exec_tracker);
        peer_pool_destroy(state->peer_pool);
        safe_free(state->peer_pool);
        dag_catalog_destroy(state->dag_catalog);
        safe_free(state->dag_catalog);
        safe_free(state->cluster_view);
        safe_free(state);
        return RESULT_ERROR(RESULT_ERR_INVALID, "Failed to initialize cluster view");
    }
    
    // ========================================================================
    // 4. Initialize Membership (Uses Cluster View)
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
        message_queue_destroy(state->message_queue);
        safe_free(state->message_queue);
        execution_tracker_destroy(state->exec_tracker);
        safe_free(state->exec_tracker);
        peer_pool_destroy(state->peer_pool);
        safe_free(state->peer_pool);
        dag_catalog_destroy(state->dag_catalog);
        safe_free(state->dag_catalog);
        safe_free(state);
        return RESULT_ERROR(RESULT_ERR_INVALID, "Failed to initialize membership");
    }
    
    // ========================================================================
    // 5. Initialize Event Bus
    // ========================================================================
    
    state->event_bus = event_bus_create();
    if (!state->event_bus) {
        membership_shutdown(state->membership);
        cluster_view_destroy(state->cluster_view);
        safe_free(state->cluster_view);
        message_queue_destroy(state->message_queue);
        safe_free(state->message_queue);
        execution_tracker_destroy(state->exec_tracker);
        safe_free(state->exec_tracker);
        peer_pool_destroy(state->peer_pool);
        safe_free(state->peer_pool);
        dag_catalog_destroy(state->dag_catalog);
        safe_free(state->dag_catalog);
        safe_free(state);
        return RESULT_ERROR(RESULT_ERR_NOMEM, "Failed to create event bus");
    }
    
    // ========================================================================
    // 6. Initialize Metrics (Optional)
    // ========================================================================
    
    if (config->ports.metrics_addr[0] != '\0') {
        if (node_metrics_init(state, config->ports.metrics_addr) != RESULT_OK) {
            LOG_WARN("Failed to initialize metrics (continuing without metrics)");
        }
    } else {
        LOG_INFO("Metrics disabled (no metrics address configured)");
    }
    
    // ========================================================================
    // 7. Register in Service Registry
    // ========================================================================
    
    service_registry_t *registry = service_registry_global();
    if (registry) {
        service_registry_register(registry, SERVICE_TYPE_NODE_STATE, 
                                 "main", state);
        service_registry_register(registry, SERVICE_TYPE_CLUSTER_VIEW,
                                 "main", state->cluster_view);
        service_registry_register(registry, SERVICE_TYPE_DAG_CATALOG,
                                 "main", state->dag_catalog);
        service_registry_register(registry, SERVICE_TYPE_EVENT_BUS,
                                 "main", state->event_bus);
        if (state->metrics_registry) {
            service_registry_register(registry, SERVICE_TYPE_METRICS,
                                     "main", state->metrics_registry);
        }
    }
    
    *out_state = state;
    
    LOG_INFO("Node state initialized successfully");
    return RESULT_SUCCESS();
}

result_t node_state_start(node_state_t *state) {
    if (!state) {
        return RESULT_ERROR(RESULT_ERR_INVALID, "NULL state");
    }
    
    LOG_INFO("Starting node services...");
    
    // Start executor threads
    if (node_start_executors(state, state->num_executor_threads) != RESULT_OK) {
        return RESULT_ERROR(RESULT_ERR_INVALID, "Failed to start executors");
    }
    
    // Start cleanup thread
    if (pthread_create(&state->cleanup_thread, NULL, cleanup_thread_fn, state) != 0) {
        LOG_ERROR("Failed to create cleanup thread");
        node_stop_executors(state);
        return RESULT_ERROR(RESULT_ERR_INVALID, "Failed to start cleanup thread");
    }
    
    // Start metrics update thread (if metrics enabled)
    if (state->metrics_registry) {
        if (pthread_create(&state->metrics_update_thread, NULL, 
                          metrics_update_thread_fn, state) != 0) {
            LOG_ERROR("Failed to create metrics update thread");
            state->shutdown_flag = 1;
            pthread_join(state->cleanup_thread, NULL);
            node_stop_executors(state);
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
        parse_addr_port(config->routers[i], seed_ip, &seed_port);
        
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
    
    // Wait for cluster view to populate (give gossip time to converge)
    LOG_INFO("Waiting for cluster view to populate...");
    for (int i = 0; i < 10; i++) {
        sleep(1);
        
        size_t member_count = state->cluster_view->count;
        if (member_count > 1) {  // Self + at least one other node
            LOG_INFO("Cluster view populated with %zu members", member_count);
            cluster_view_dump(state->cluster_view, "After Bootstrap");
            return RESULT_SUCCESS();
        }
    }
    
    LOG_WARN("Cluster view not fully populated after 10s (may be normal for slow networks)");
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
    
    // Stop executor threads
    node_stop_executors(state);
    
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
    
    if (state->message_queue) {
        message_queue_destroy(state->message_queue);
        safe_free(state->message_queue);
        state->message_queue = NULL;
    }
    
    if (state->exec_tracker) {
        execution_tracker_destroy(state->exec_tracker);
        safe_free(state->exec_tracker);
        state->exec_tracker = NULL;
    }
    
    if (state->peer_pool) {
        peer_pool_destroy(state->peer_pool);
        safe_free(state->peer_pool);
        state->peer_pool = NULL;
    }
    
    if (state->dag_catalog) {
        dag_catalog_destroy(state->dag_catalog);
        safe_free(state->dag_catalog);
        state->dag_catalog = NULL;
    }
    
    safe_free(state);
    
    LOG_INFO("Node state destroyed");
}

// ============================================================================
// ACCESSOR FUNCTIONS (Already Declared in Header)
// ============================================================================

const node_identity_t* node_state_get_identity(const node_state_t *state) {
    return state ? &state->identity : NULL;
}

const node_capabilities_t* node_state_get_capabilities(const node_state_t *state) {
    return state ? &state->capabilities : NULL;
}

dag_catalog_t* node_state_get_dag_catalog(node_state_t *state) {
    return state ? state->dag_catalog : NULL;
}

peer_pool_t* node_state_get_peer_pool(node_state_t *state) {
    return state ? state->peer_pool : NULL;
}

execution_tracker_t* node_state_get_exec_tracker(node_state_t *state) {
    return state ? state->exec_tracker : NULL;
}

message_queue_t* node_state_get_message_queue(node_state_t *state) {
    return state ? state->message_queue : NULL;
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
    stats->active_executions = state->active_executions;
    stats->messages_processed = state->messages_processed;
    stats->messages_failed = state->messages_failed;
    stats->messages_routed = state->messages_routed;
    
    if (state->message_queue) {
        stats->queue_depth = message_queue_size(state->message_queue);
    }
    
    if (state->cluster_view) {
        stats->cluster_size = state->cluster_view->count;
    }
}
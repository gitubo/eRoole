// src/node/node_main.c
// Simplified node executable - Pure distributed datastore

#define _POSIX_C_SOURCE 200809L

#include "roole/node/node_state.h"
#include "roole/node/node_rpc.h"
#include "roole/config/config.h"
#include "roole/config/config_validator.h"
#include "roole/core/service_registry.h"
#include "roole/core/common.h"
#include "roole/logger/logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>

// ============================================================================
// GLOBAL STATE FOR SIGNAL HANDLING
// ============================================================================

static volatile sig_atomic_t g_shutdown_requested = 0;
static node_state_t *g_node_state = NULL;

// ============================================================================
// SIGNAL HANDLERS
// ============================================================================

static void signal_handler(int signum) {
    const char *sig_name = (signum == SIGINT) ? "SIGINT" : 
                          (signum == SIGTERM) ? "SIGTERM" : "UNKNOWN";
    
    fprintf(stderr, "\n[SIGNAL] Received %s - initiating graceful shutdown\n", sig_name);
    
    g_shutdown_requested = 1;
    
    if (g_node_state) {
        g_node_state->shutdown_flag = 1;
    }
}

static void setup_signal_handlers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    
    if (sigaction(SIGINT, &sa, NULL) < 0) {
        LOG_ERROR("Failed to register SIGINT handler");
    }
    
    if (sigaction(SIGTERM, &sa, NULL) < 0) {
        LOG_ERROR("Failed to register SIGTERM handler");
    }
    
    signal(SIGPIPE, SIG_IGN);
    
    LOG_INFO("Signal handlers registered (SIGINT, SIGTERM)");
}

// ============================================================================
// CONFIGURATION DISPLAY
// ============================================================================

static void display_configuration(const roole_config_t *config) {
    LOG_INFO("========================================");
    LOG_INFO("Node Configuration:");
    LOG_INFO("  Cluster: %s", config->cluster_name);
    LOG_INFO("  Node ID: %u", config->node_id);
    LOG_INFO("  Node Type: %s", 
             config->node_type == NODE_TYPE_ROUTER ? "ROUTER" : "WORKER");
    LOG_INFO("  Gossip: %s", config->ports.gossip_addr);
    LOG_INFO("  Data: %s", config->ports.data_addr);
    
    if (config->ports.ingress_addr[0] != '\0') {
        LOG_INFO("  Ingress: %s (client-facing)", config->ports.ingress_addr);
    } else {
        LOG_INFO("  Ingress: DISABLED");
    }
    
    if (config->ports.metrics_addr[0] != '\0') {
        LOG_INFO("  Metrics: %s", config->ports.metrics_addr);
    } else {
        LOG_INFO("  Metrics: DISABLED");
    }
    
    if (config->router_count > 0) {
        LOG_INFO("  Seed Routers: %zu", config->router_count);
        for (size_t i = 0; i < config->router_count; i++) {
            LOG_INFO("    - %s", config->routers[i]);
        }
    } else {
        LOG_INFO("  Seed Routers: NONE (standalone/seed node)");
    }
    
    LOG_INFO("  Log Level: %s", 
             config->log_level == LOG_LEVEL_DEBUG ? "DEBUG" :
             config->log_level == LOG_LEVEL_INFO ? "INFO" :
             config->log_level == LOG_LEVEL_WARN ? "WARN" : "ERROR");
    LOG_INFO("========================================");
}

// ============================================================================
// GRACEFUL SHUTDOWN
// ============================================================================

static void perform_graceful_shutdown(node_state_t *state) {
    LOG_INFO("========================================");
    LOG_INFO("GRACEFUL SHUTDOWN SEQUENCE");
    LOG_INFO("========================================");
    
    // Phase 1: Stop accepting new work
    LOG_INFO("[1/3] Stopping acceptance of new requests...");
    state->shutdown_flag = 1;
    sleep(1);
    LOG_INFO("  ✓ No longer accepting new requests");
    
    // Phase 2: Leave cluster gracefully
    LOG_INFO("[2/3] Leaving cluster...");
    if (state->membership) {
        membership_leave(state->membership);
        sleep(2);  // Give time for LEAVE message to propagate
        LOG_INFO("  ✓ Graceful LEAVE sent to cluster");
    } else {
        LOG_INFO("  ⓘ No membership to leave");
    }
    
    // Phase 3: Shutdown services
    LOG_INFO("[3/3] Shutting down node services...");
    node_state_shutdown(state);
    LOG_INFO("  ✓ All services stopped");
    
    // Display final statistics
    node_statistics_t stats;
    node_state_get_statistics(state, &stats);
    
    LOG_INFO("========================================");
    LOG_INFO("Final Statistics:");
    LOG_INFO("  Uptime: %lu seconds", stats.uptime_ms / 1000);
    LOG_INFO("  Datastore Records: %zu", stats.datastore_records);
    LOG_INFO("  Datastore Bytes: %zu", stats.datastore_bytes);
    LOG_INFO("  Total Operations: %lu", stats.datastore_ops_total);
    LOG_INFO("  Cluster Size: %zu nodes", stats.cluster_size);
    LOG_INFO("========================================");
}

// ============================================================================
// MAIN ENTRY POINT
// ============================================================================

int main(int argc, char **argv) {
    // Parse command line arguments
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <config_file>\n", argv[0]);
        fprintf(stderr, "\n");
        fprintf(stderr, "Arguments:\n");
        fprintf(stderr, "  config_file  - Path to INI configuration file\n");
        fprintf(stderr, "\n");
        fprintf(stderr, "Examples:\n");
        fprintf(stderr, "  %s ./config/router.ini\n", argv[0]);
        fprintf(stderr, "  %s ./config/worker_100.ini\n", argv[0]);
        return 1;
    }
    
    const char *config_path = argv[1];
    
    // ========================================================================
    // INITIALIZATION PHASE
    // ========================================================================
    
    printf("\n");
    printf("╔════════════════════════════════════════╗\n");
    printf("║  ROOLE DISTRIBUTED DATASTORE v2.0     ║\n");
    printf("║  Pure Key-Value Storage Cluster       ║\n");
    printf("╚════════════════════════════════════════╝\n");
    printf("\n");
    
    // Initialize logger
    logger_init();
    logger_set_level(LOG_LEVEL_INFO);
    
    LOG_INFO("Starting Roole datastore node...");
    LOG_INFO("  Config file: %s", config_path);
    
    // Load configuration
    roole_config_t config;
    if (config_load_from_file(config_path, &config) != 0) {
        LOG_ERROR("Failed to load configuration from %s", config_path);
        logger_shutdown();
        return 1;
    }
    
    LOG_INFO("✓ Configuration loaded");
    
    // Set logger context
    const char *node_type_str = (config.node_type == NODE_TYPE_ROUTER) ? 
                                 "router" : "worker";
    logger_set_context(config.node_id, config.cluster_name, node_type_str);
    
    // Display configuration
    display_configuration(&config);
    LOG_INFO("✓ Configuration validated");
    
    // Create global service registry
    service_registry_t *registry = service_registry_create();
    if (!registry) {
        LOG_ERROR("Failed to create service registry");
        logger_shutdown();
        return 1;
    }
    service_registry_set_global(registry);
    LOG_INFO("✓ Service registry created");
    
    // ========================================================================
    // NODE INITIALIZATION
    // ========================================================================
    
    LOG_INFO("Initializing node state (pure datastore mode)...");
    
    node_state_t *state = NULL;
    result_t init_result = node_state_init(&state, &config);
    
    if (result_is_error(&init_result)) {
        LOG_ERROR("Failed to initialize node state:");
        result_log_error(&init_result);
        service_registry_destroy(registry);
        logger_shutdown();
        return 1;
    }
    
    g_node_state = state;
    
    LOG_INFO("✓ Node state initialized");
    
    // Register node state in service registry
    service_registry_register(registry, SERVICE_TYPE_NODE_STATE, "main", state);
    
    // ========================================================================
    // START SERVICES
    // ========================================================================
    
    LOG_INFO("Starting node services...");
    
    result_t start_result = node_state_start(state);
    if (result_is_error(&start_result)) {
        LOG_ERROR("Failed to start node services:");
        result_log_error(&start_result);
        node_state_destroy(state);
        service_registry_destroy(registry);
        logger_shutdown();
        return 1;
    }
    
    LOG_INFO("✓ Background threads started");
    
    // Setup signal handlers
    setup_signal_handlers();
    
    // ========================================================================
    // START RPC SERVERS
    // ========================================================================
    
    LOG_INFO("Starting RPC servers...");
    
    if (node_start_rpc_servers(state) != 0) {
        LOG_ERROR("Failed to start RPC servers");
        node_state_shutdown(state);
        node_state_destroy(state);
        service_registry_destroy(registry);
        logger_shutdown();
        return 1;
    }
    
    LOG_INFO("✓ RPC servers started");
    
    sleep(1);  // Brief pause to ensure servers are bound
    
    // ========================================================================
    // CLUSTER BOOTSTRAP
    // ========================================================================
    
    LOG_INFO("Bootstrapping cluster membership...");
    
    result_t bootstrap_result = node_state_bootstrap(state, &config);
    
    if (result_is_error(&bootstrap_result)) {
        LOG_WARN("Bootstrap warning:");
        result_log_error(&bootstrap_result);
        LOG_INFO("Continuing - node will discover cluster via gossip");
    } else {
        LOG_INFO("✓ Bootstrap completed successfully");
    }
    
    // ========================================================================
    // OPERATIONAL PHASE
    // ========================================================================
    
    LOG_INFO("========================================");
    LOG_INFO("NODE FULLY OPERATIONAL");
    LOG_INFO("========================================");
    LOG_INFO("  Node ID: %u", state->identity.node_id);
    LOG_INFO("  Cluster: %s", state->identity.cluster_name);
    LOG_INFO("  Type: %s", 
             state->capabilities.has_ingress ? "ROUTER" : "WORKER");
    LOG_INFO("");
    LOG_INFO("Endpoints:");
    LOG_INFO("  Gossip: %s:%u", state->identity.bind_addr, 
             state->identity.gossip_port);
    LOG_INFO("  Data: %s:%u", state->identity.bind_addr, 
             state->identity.data_port);
    
    if (state->capabilities.has_ingress) {
        LOG_INFO("  Ingress: %s:%u (client requests)", 
                 state->identity.bind_addr, state->identity.ingress_port);
    }
    
    if (state->identity.metrics_port > 0) {
        LOG_INFO("  Metrics: http://%s:%u/metrics", 
                 state->identity.bind_addr, state->identity.metrics_port);
    }
    
    LOG_INFO("========================================");
    LOG_INFO("Distributed Key-Value Datastore Ready");
    LOG_INFO("Operations: SET, GET, UNSET, LIST");
    LOG_INFO("========================================");
    LOG_INFO("Press Ctrl+C to initiate graceful shutdown");
    LOG_INFO("========================================");
    
    // Main loop - periodic status updates
    uint64_t last_status_log = time_now_ms();
    const uint64_t status_interval_ms = 60000;  // 60 seconds
    
    while (!g_shutdown_requested) {
        sleep(1);
        
        uint64_t now = time_now_ms();
        if (now - last_status_log > status_interval_ms) {
            node_statistics_t stats;
            node_state_get_statistics(state, &stats);
            
            LOG_INFO("Status: uptime=%lus | records=%zu | bytes=%zu | cluster=%zu",
                    stats.uptime_ms / 1000,
                    stats.datastore_records,
                    stats.datastore_bytes,
                    stats.cluster_size);
            
            last_status_log = now;
        }
    }
    
    // ========================================================================
    // SHUTDOWN PHASE
    // ========================================================================
    
    perform_graceful_shutdown(state);
    
    // Destroy node state
    LOG_INFO("Destroying node state...");
    node_state_destroy(state);
    LOG_INFO("✓ Node state destroyed");
    
    // Cleanup service registry
    service_registry_destroy(registry);
    
    LOG_INFO("========================================");
    LOG_INFO("SHUTDOWN COMPLETE");
    LOG_INFO("========================================");
    
    logger_shutdown();
    
    printf("\n");
    printf("╔════════════════════════════════════════╗\n");
    printf("║          Shutdown Complete             ║\n");
    printf("║            Goodbye! 👋                 ║\n");
    printf("╚════════════════════════════════════════╝\n");
    printf("\n");
    
    return 0;
}
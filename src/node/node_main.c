// src/node/node_main.c
// Production-ready node executable with proper lifecycle management

#define _POSIX_C_SOURCE 200809L

#include "roole/node/node_state.h"
#include "roole/node/node_rpc.h"
#include "roole/node/node_executor.h"
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
    
    // Signal the node to begin shutdown
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
    
    // Ignore SIGPIPE (broken pipe on socket write)
    signal(SIGPIPE, SIG_IGN);
    
    LOG_INFO("Signal handlers registered (SIGINT, SIGTERM)");
}

// ============================================================================
// CONFIGURATION VALIDATION AND DISPLAY
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

static int validate_and_display_config(const roole_config_t *config) {
    validation_result_t validation;
    validation_result_init(&validation);
    
    if (config_validate(config, &validation) != 0) {
        LOG_ERROR("Configuration validation failed:");
        validation_result_print(&validation);
        return -1;
    }
    
    if (validation.error_count > 0) {
        LOG_WARN("Configuration has warnings:");
        validation_result_print(&validation);
    }
    
    display_configuration(config);
    return 0;
}

// ============================================================================
// GRACEFUL SHUTDOWN
// ============================================================================

static void perform_graceful_shutdown(node_state_t *state) {
    LOG_INFO("========================================");
    LOG_INFO("GRACEFUL SHUTDOWN SEQUENCE");
    LOG_INFO("========================================");
    
    // Phase 1: Stop accepting new work
    LOG_INFO("[1/6] Stopping acceptance of new work...");
    state->shutdown_flag = 1;
    sleep(1);  // Brief pause for in-flight RPC requests
    LOG_INFO("  ✓ No longer accepting new work");
    
    // Phase 2: Drain message queue
    LOG_INFO("[2/6] Draining message queue...");
    message_queue_t *queue = node_state_get_message_queue(state);
    int drain_attempts = 0;
    const int max_drain_attempts = 30;  // 30 seconds max
    
    while (!message_queue_is_empty(queue) && drain_attempts < max_drain_attempts) {
        size_t queue_size = message_queue_size(queue);
        LOG_DEBUG("  Queue size: %zu, waiting...", queue_size);
        sleep(1);
        drain_attempts++;
    }
    
    if (message_queue_is_empty(queue)) {
        LOG_INFO("  ✓ Message queue drained");
    } else {
        LOG_WARN("  ⚠ Queue not empty after %d seconds, proceeding anyway", 
                max_drain_attempts);
    }
    
    // Phase 3: Wait for active executions
    LOG_INFO("[3/6] Waiting for active executions to complete...");
    int exec_wait_attempts = 0;
    const int max_exec_wait = 60;  // 60 seconds max
    
    while (state->active_executions > 0 && exec_wait_attempts < max_exec_wait) {
        LOG_DEBUG("  Active executions: %u", state->active_executions);
        sleep(1);
        exec_wait_attempts++;
    }
    
    if (state->active_executions == 0) {
        LOG_INFO("  ✓ All executions completed");
    } else {
        LOG_WARN("  ⚠ %u executions still active after %d seconds, forcing shutdown", 
                state->active_executions, max_exec_wait);
    }
    
    // Phase 4: Stop executor threads
    LOG_INFO("[4/6] Stopping executor threads...");
    node_stop_executors(state);
    LOG_INFO("  ✓ Executor threads stopped");
    
    // Phase 5: Leave cluster gracefully
    LOG_INFO("[5/6] Leaving cluster...");
    if (state->membership) {
        membership_leave(state->membership);
        sleep(2);  // Give time for LEAVE message to propagate
        LOG_INFO("  ✓ Graceful LEAVE sent to cluster");
    } else {
        LOG_INFO("  ⓘ No membership to leave");
    }
    
    // Phase 6: Shutdown services
    LOG_INFO("[6/6] Shutting down node services...");
    node_state_shutdown(state);
    LOG_INFO("  ✓ All services stopped");
    
    // Display final statistics
    node_statistics_t stats;
    node_state_get_statistics(state, &stats);
    
    LOG_INFO("========================================");
    LOG_INFO("Final Statistics:");
    LOG_INFO("  Uptime: %lu seconds", stats.uptime_ms / 1000);
    LOG_INFO("  Messages Processed: %lu", stats.messages_processed);
    LOG_INFO("  Messages Failed: %lu", stats.messages_failed);
    LOG_INFO("  Messages Routed: %lu", stats.messages_routed);
    LOG_INFO("  Cluster Size: %zu nodes", stats.cluster_size);
    LOG_INFO("========================================");
}

// ============================================================================
// MAIN ENTRY POINT
// ============================================================================

int main(int argc, char **argv) {
    // Parse command line arguments
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <config_file> <num_executor_threads>\n", argv[0]);
        fprintf(stderr, "\n");
        fprintf(stderr, "Arguments:\n");
        fprintf(stderr, "  config_file           - Path to INI configuration file\n");
        fprintf(stderr, "  num_executor_threads  - Number of executor threads (1-128)\n");
        fprintf(stderr, "\n");
        fprintf(stderr, "Examples:\n");
        fprintf(stderr, "  %s ./config/router.ini 2\n", argv[0]);
        fprintf(stderr, "  %s ./config/worker_100.ini 4\n", argv[0]);
        return 1;
    }
    
    const char *config_path = argv[1];
    size_t num_executor_threads = (size_t)atoi(argv[2]);
    
    if (num_executor_threads == 0) {
        fprintf(stderr, "ERROR: Invalid number of executor threads: %s\n", argv[2]);
        fprintf(stderr, "Must be between 1 and 128\n");
        return 1;
    }
    
    if (num_executor_threads > 128) {
        fprintf(stderr, "WARNING: Limiting executor threads from %zu to 128\n", 
                num_executor_threads);
        num_executor_threads = 128;
    }
    
    // ========================================================================
    // INITIALIZATION PHASE
    // ========================================================================
    
    printf("\n");
    printf("╔════════════════════════════════════════╗\n");
    printf("║     ROOLE DISTRIBUTED NODE v1.0        ║\n");
    printf("╚════════════════════════════════════════╝\n");
    printf("\n");
    
    // Initialize logger
    logger_init();
    logger_set_level(LOG_LEVEL_DEBUG);
    
    LOG_INFO("Starting Roole node...");
    LOG_INFO("  Config file: %s", config_path);
    LOG_INFO("  Executor threads: %zu", num_executor_threads);
    
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
    
    // Validate configuration
    if (validate_and_display_config(&config) != 0) {
        logger_shutdown();
        return 1;
    }
    
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
    
    LOG_INFO("Initializing node state...");
    
    node_state_t *state = NULL;
    result_t init_result = node_state_init(&state, &config, num_executor_threads);
    
    if (result_is_error(&init_result)) {
        LOG_ERROR("Failed to initialize node state:");
        result_log_error(&init_result);
        service_registry_destroy(registry);
        logger_shutdown();
        return 1;
    }
    
    g_node_state = state;  // Set global for signal handler
    
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
    
    LOG_INFO("✓ Executor threads started (%zu)", num_executor_threads);
    LOG_INFO("✓ Cleanup thread started");
    LOG_INFO("✓ Metrics update thread started");
    
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
    
    // Brief pause to ensure RPC servers are fully bound
    sleep(1);
    
    // ========================================================================
    // CLUSTER BOOTSTRAP
    // ========================================================================
    
    LOG_INFO("Bootstrapping cluster membership...");
    
    result_t bootstrap_result = node_state_bootstrap(state, &config);
    
    if (result_is_error(&bootstrap_result)) {
        LOG_WARN("Bootstrap failed or timed out:");
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
        LOG_INFO("  Ingress: %s:%u (accepting client requests)", 
                 state->identity.bind_addr, state->identity.ingress_port);
    }
    
    if (state->identity.metrics_port > 0) {
        LOG_INFO("  Metrics: http://%s:%u/metrics", 
                 state->identity.bind_addr, state->identity.metrics_port);
    }
    
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
            
            LOG_INFO("Status: uptime=%lus | processed=%lu | failed=%lu | "
                    "queue=%zu | active=%u | cluster=%zu",
                    stats.uptime_ms / 1000,
                    stats.messages_processed,
                    stats.messages_failed,
                    stats.queue_depth,
                    stats.active_executions,
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
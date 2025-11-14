// src/node/node_rpc.c
// Simplified RPC server startup using handler registry

#define _POSIX_C_SOURCE 200809L

#include "roole/node/node_rpc.h"
#include "roole/node/node_handlers.h"
#include "roole/rpc/rpc_server.h"
#include "roole/core/common.h"
#include <pthread.h>
#include <unistd.h>

// ============================================================================
// RPC SERVER THREADS
// ============================================================================

typedef struct rpc_server_context {
    node_state_t *state;
    rpc_server_t *server;
    const char *server_type;  // "DATA" or "INGRESS"
} rpc_server_context_t;

static void* rpc_server_thread_fn(void *arg) {
    rpc_server_context_t *ctx = (rpc_server_context_t*)arg;
    
    logger_push_component("rpc");
    LOG_INFO("%s RPC server thread started", ctx->server_type);
    
    // Run server (blocks until stopped)
    rpc_server_run(ctx->server);
    
    LOG_INFO("%s RPC server thread stopped", ctx->server_type);
    logger_pop_component();
    
    return NULL;
}

// ============================================================================
// PUBLIC API
// ============================================================================

int node_start_rpc_servers(node_state_t *state) {
    if (!state) {
        LOG_ERROR("Cannot start RPC servers: NULL state");
        return -1;
    }
    
    const node_identity_t *identity = node_state_get_identity(state);
    const node_capabilities_t *caps = node_state_get_capabilities(state);
    
    // Build handler registry
    rpc_handler_registry_t *registry = node_build_handler_registry(state);
    if (!registry) {
        LOG_ERROR("Failed to build handler registry");
        return -1;
    }
    
    LOG_INFO("========================================");
    LOG_INFO("Starting RPC Servers");
    LOG_INFO("  Node: %u (%s)", identity->node_id, identity->cluster_name);
    LOG_INFO("  DATA port: %u (always present)", identity->data_port);
    
    if (caps->has_ingress) {
        LOG_INFO("  INGRESS port: %u (client-facing)", identity->ingress_port);
    } else {
        LOG_INFO("  INGRESS: disabled");
    }
    LOG_INFO("========================================");
    
    // Configure DATA server (always present)
    rpc_server_config_t data_config = {
        .port = identity->data_port,
        .bind_addr = identity->bind_addr,
        .channel_type = RPC_CHANNEL_DATA,
        .max_connections = 512,
        .buffer_size = 8192,
        .recv_timeout_ms = 5000
    };
    
    rpc_server_t *data_server = rpc_server_create(&data_config, registry);
    if (!data_server) {
        LOG_ERROR("Failed to create DATA RPC server");
        rpc_handler_registry_destroy(registry);
        return -1;
    }
    
    LOG_INFO("DATA RPC server created successfully");
    
    // Start DATA server thread
    pthread_t data_thread;
    rpc_server_context_t data_ctx = {
        .state = state,
        .server = data_server,
        .server_type = "DATA"
    };
    
    if (pthread_create(&data_thread, NULL, rpc_server_thread_fn, &data_ctx) != 0) {
        LOG_ERROR("Failed to create DATA server thread");
        rpc_server_destroy(data_server);
        rpc_handler_registry_destroy(registry);
        return -1;
    }
    
    pthread_detach(data_thread);
    LOG_INFO("DATA RPC server thread started");
    
    // Configure INGRESS server (if has_ingress capability)
    if (caps->has_ingress) {
        rpc_server_config_t ingress_config = {
            .port = identity->ingress_port,
            .bind_addr = identity->bind_addr,
            .channel_type = RPC_CHANNEL_INGRESS,
            .max_connections = 1024,
            .buffer_size = 8192,
            .recv_timeout_ms = 10000
        };
        
        rpc_server_t *ingress_server = rpc_server_create(&ingress_config, registry);
        if (!ingress_server) {
            LOG_ERROR("Failed to create INGRESS RPC server");
            // DATA server already running, but continue
        } else {
            LOG_INFO("INGRESS RPC server created successfully");
            
            // Start INGRESS server thread
            pthread_t ingress_thread;
            rpc_server_context_t ingress_ctx = {
                .state = state,
                .server = ingress_server,
                .server_type = "INGRESS"
            };
            
            if (pthread_create(&ingress_thread, NULL, rpc_server_thread_fn, &ingress_ctx) != 0) {
                LOG_ERROR("Failed to create INGRESS server thread");
                rpc_server_destroy(ingress_server);
            } else {
                pthread_detach(ingress_thread);
                LOG_INFO("INGRESS RPC server thread started");
            }
        }
    }
    
    LOG_INFO("RPC servers startup complete");
    
    // Give servers time to bind and listen
    sleep(1);
    
    return 0;
}
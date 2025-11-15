// src/node/handlers/handler_registry.c
// Simplified handler registry - Datastore operations only

#define _POSIX_C_SOURCE 200809L

#include "roole/node/node_handlers.h"
#include "roole/node/node_state.h"
#include "roole/rpc/rpc_handler.h"
#include "roole/rpc/rpc_types.h"
#include "roole/core/common.h"

// ============================================================================
// BUILD HANDLER REGISTRY (SIMPLIFIED)
// ============================================================================

rpc_handler_registry_t* node_build_handler_registry(node_state_t *state) {
    if (!state) {
        LOG_ERROR("Cannot build handler registry: NULL state");
        return NULL;
    }
    
    const node_capabilities_t *caps = node_state_get_capabilities(state);
    
    LOG_INFO("Building RPC handler registry (has_ingress=%d)",
             caps->has_ingress);
    
    // Create registry
    rpc_handler_registry_t *registry = rpc_handler_registry_create();
    if (!registry) {
        LOG_ERROR("Failed to create handler registry");
        return NULL;
    }
    
    // ========================================================================
    // Register INGRESS handlers (client-facing, only if has_ingress)
    // ========================================================================
    
    if (caps->has_ingress) {
        LOG_INFO("Registering INGRESS handlers (client-facing datastore ops)");
        
        if (rpc_handler_register(registry, FUNC_ID_DATASTORE_SET,
                                handle_datastore_set, state) != 0) {
            LOG_ERROR("Failed to register DATASTORE_SET handler");
            rpc_handler_registry_destroy(registry);
            return NULL;
        }
        
        if (rpc_handler_register(registry, FUNC_ID_DATASTORE_GET,
                                handle_datastore_get, state) != 0) {
            LOG_ERROR("Failed to register DATASTORE_GET handler");
            rpc_handler_registry_destroy(registry);
            return NULL;
        }
        
        if (rpc_handler_register(registry, FUNC_ID_DATASTORE_UNSET,
                                handle_datastore_unset, state) != 0) {
            LOG_ERROR("Failed to register DATASTORE_UNSET handler");
            rpc_handler_registry_destroy(registry);
            return NULL;
        }
        
        if (rpc_handler_register(registry, FUNC_ID_DATASTORE_LIST,
                                handle_datastore_list_keys, state) != 0) {
            LOG_ERROR("Failed to register DATASTORE_LIST handler");
            rpc_handler_registry_destroy(registry);
            return NULL;
        }
        
        LOG_INFO("Registered 4 INGRESS handlers (SET, GET, UNSET, LIST)");
    } else {
        LOG_INFO("Skipping INGRESS handlers (no ingress capability)");
    }
    
    // ========================================================================
    // Register DATA handlers (peer-to-peer, always present)
    // ========================================================================
    
    LOG_INFO("Registering DATA handlers (peer-to-peer datastore sync)");
    
    if (rpc_handler_register(registry, FUNC_ID_DATASTORE_SYNC,
                            handle_datastore_sync, state) != 0) {
        LOG_ERROR("Failed to register DATASTORE_SYNC handler");
        rpc_handler_registry_destroy(registry);
        return NULL;
    }
    
    LOG_INFO("Registered 1 DATA handler (SYNC)");
    
    // ========================================================================
    // Summary
    // ========================================================================
    
    size_t total_handlers = caps->has_ingress ? 5 : 1;
    LOG_INFO("Handler registry built successfully: %zu total handlers", total_handlers);
    
    return registry;
}
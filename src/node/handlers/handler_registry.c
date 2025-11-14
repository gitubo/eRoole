#define _POSIX_C_SOURCE 200809L

#include "roole/node/node_handlers.h"
#include "roole/node/node_state.h"
#include "roole/rpc/rpc_handler.h"
#include "roole/rpc/rpc_types.h"
#include "roole/core/common.h"

// ============================================================================
// BUILD HANDLER REGISTRY
// ============================================================================

rpc_handler_registry_t* node_build_handler_registry(node_state_t *state) {
    if (!state) {
        LOG_ERROR("Cannot build handler registry: NULL state");
        return NULL;
    }
    
    const node_capabilities_t *caps = node_state_get_capabilities(state);
    
    LOG_INFO("Building RPC handler registry (has_ingress=%d, can_execute=%d, can_route=%d)",
             caps->has_ingress, caps->can_execute, caps->can_route);
    
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
        LOG_INFO("Registering INGRESS handlers (client-facing)");
        
        if (rpc_handler_register(registry, FUNC_ID_SUBMIT_MESSAGE,
                                handle_submit_message, state) != 0) {
            LOG_ERROR("Failed to register SUBMIT_MESSAGE handler");
            rpc_handler_registry_destroy(registry);
            return NULL;
        }
        
        if (rpc_handler_register(registry, FUNC_ID_GET_STATUS,
                                handle_get_execution_status, state) != 0) {
            LOG_ERROR("Failed to register GET_STATUS handler");
            rpc_handler_registry_destroy(registry);
            return NULL;
        }
        
        if (rpc_handler_register(registry, FUNC_ID_LIST_DAGS,
                                handle_list_dags, state) != 0) {
            LOG_ERROR("Failed to register LIST_DAGS handler");
            rpc_handler_registry_destroy(registry);
            return NULL;
        }
        
        if (rpc_handler_register(registry, FUNC_ID_ADD_DAG,
                                handle_add_dag, state) != 0) {
            LOG_ERROR("Failed to register ADD_DAG handler");
            rpc_handler_registry_destroy(registry);
            return NULL;
        }
        
        LOG_INFO("Registered 4 INGRESS handlers");
    } else {
        LOG_INFO("Skipping INGRESS handlers (no ingress capability)");
    }
    
    // ========================================================================
    // Register DATA handlers (peer-to-peer, always present)
    // ========================================================================
    
    LOG_INFO("Registering DATA handlers (peer-to-peer)");
    
    if (rpc_handler_register(registry, FUNC_ID_PROCESS_MESSAGE,
                            handle_process_message, state) != 0) {
        LOG_ERROR("Failed to register PROCESS_MESSAGE handler");
        rpc_handler_registry_destroy(registry);
        return NULL;
    }
    
    if (rpc_handler_register(registry, FUNC_ID_EXECUTION_UPDATE,
                            handle_execution_update, state) != 0) {
        LOG_ERROR("Failed to register EXECUTION_UPDATE handler");
        rpc_handler_registry_destroy(registry);
        return NULL;
    }
    
    if (rpc_handler_register(registry, FUNC_ID_SYNC_CATALOG,
                            handle_sync_catalog, state) != 0) {
        LOG_ERROR("Failed to register SYNC_CATALOG handler");
        rpc_handler_registry_destroy(registry);
        return NULL;
    }
    
    LOG_INFO("Registered 3 DATA handlers");
    
    // ========================================================================
    // Summary
    // ========================================================================
    
    size_t total_handlers = caps->has_ingress ? 7 : 3;
    LOG_INFO("Handler registry built successfully: %zu total handlers", total_handlers);
    
    return registry;
}
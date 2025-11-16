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

        if (rpc_handler_register(registry, FUNC_ID_RAFT_KV_SET,
                                handle_raft_kv_set, state) != 0) {
            LOG_ERROR("Failed to register RAFT_KV_SET handler");
            rpc_handler_registry_destroy(registry);
            return NULL;
        }
        
        if (rpc_handler_register(registry, FUNC_ID_RAFT_KV_GET,
                                handle_raft_kv_get, state) != 0) {
            LOG_ERROR("Failed to register RAFT_KV_GET handler");
            rpc_handler_registry_destroy(registry);
            return NULL;
        }
        
        if (rpc_handler_register(registry, FUNC_ID_RAFT_KV_UNSET,
                                handle_raft_kv_unset, state) != 0) {
            LOG_ERROR("Failed to register RAFT_KV_UNSET handler");
            rpc_handler_registry_destroy(registry);
            return NULL;
        }
        
        if (rpc_handler_register(registry, FUNC_ID_RAFT_KV_LIST,
                                handle_raft_kv_list, state) != 0) {
            LOG_ERROR("Failed to register RAFT_KV_LIST handler");
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
    
    if (rpc_handler_register(registry, FUNC_ID_RAFT_STATUS,
                            handle_rafk_kv_status, state) != 0) {
        LOG_ERROR("Failed to register RAFT_STATUS handler");
        rpc_handler_registry_destroy(registry);
        return NULL;
    }

    LOG_INFO("Registered 1 DATA handler (SYNC)");    

    // ========================================================================
    // Register RAFT CONSENSUS handlers (peer-to-peer, always present)
    // ========================================================================
    
    if (state->raft_state) {
        LOG_INFO("Registering Raft consensus handlers (peer-to-peer)");
        
        if (raft_register_handlers(registry, state->raft_state) != 0) {
            LOG_ERROR("Failed to register Raft consensus handlers");
            rpc_handler_registry_destroy(registry);
            return NULL;
        }
        
        LOG_INFO("Registered 3 Raft consensus handlers (RequestVote, AppendEntries, InstallSnapshot)");
    }
    
    // ========================================================================
    // Register RAFT DATASTORE handlers (ingress only, if has_ingress)
    // ========================================================================
    
    if (caps->has_ingress && state->raft_datastore) {
        LOG_INFO("Registering Raft datastore handlers (client-facing)");
        
        if (rpc_handler_register(registry, FUNC_ID_RAFT_KV_SET,
                                handle_raft_kv_set, state) != 0) {
            LOG_ERROR("Failed to register RAFT_KV_SET handler");
            rpc_handler_registry_destroy(registry);
            return NULL;
        }
        
        if (rpc_handler_register(registry, FUNC_ID_RAFT_KV_GET,
                                handle_raft_kv_get, state) != 0) {
            LOG_ERROR("Failed to register RAFT_KV_GET handler");
            rpc_handler_registry_destroy(registry);
            return NULL;
        }
        
        if (rpc_handler_register(registry, FUNC_ID_RAFT_KV_UNSET,
                                handle_raft_kv_unset, state) != 0) {
            LOG_ERROR("Failed to register RAFT_KV_UNSET handler");
            rpc_handler_registry_destroy(registry);
            return NULL;
        }
        
        if (rpc_handler_register(registry, FUNC_ID_RAFT_KV_LIST,
                                handle_raft_kv_list, state) != 0) {
            LOG_ERROR("Failed to register RAFT_KV_LIST handler");
            rpc_handler_registry_destroy(registry);
            return NULL;
        }
        
        if (rpc_handler_register(registry, FUNC_ID_RAFT_STATUS,
                                handle_raft_status, state) != 0) {
            LOG_ERROR("Failed to register RAFT_STATUS handler");
            rpc_handler_registry_destroy(registry);
            return NULL;
        }
        
        LOG_INFO("Registered 5 Raft datastore handlers (SET, GET, UNSET, LIST, STATUS)");
    } else if (state->raft_datastore) {
        LOG_INFO("Skipping Raft datastore ingress handlers (no ingress capability)");
    }
    
    // ========================================================================
    // Summary
    // ========================================================================
    
    size_t total_handlers = caps->has_ingress ? 5 : 1;
    LOG_INFO("Handler registry built successfully: %zu total handlers", total_handlers);
    
    return registry;
}
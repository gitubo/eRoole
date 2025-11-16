// src/node/handlers/handler_registry.c
// CORRECTED: Fixed typo, removed duplicate, proper separation of concerns

#define _POSIX_C_SOURCE 200809L

#include "roole/node/node_handlers.h"
#include "roole/node/node_state.h"
#include "roole/raft/raft_rpc.h"
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
    
    LOG_INFO("========================================");
    LOG_INFO("Building RPC Handler Registry");
    LOG_INFO("  Node: %u", state->identity.node_id);
    LOG_INFO("  Has Ingress: %s", caps->has_ingress ? "YES" : "NO");
    LOG_INFO("  Raft Enabled: %s", state->raft_state ? "YES" : "NO");
    LOG_INFO("========================================");
    
    // Create registry
    rpc_handler_registry_t *registry = rpc_handler_registry_create();
    if (!registry) {
        LOG_ERROR("Failed to create handler registry");
        return NULL;
    }
    
    // ========================================================================
    // PEER-TO-PEER HANDLERS (DATA channel - always present)
    // These are the CORE RAFT CONSENSUS RPCs (from the paper)
    // ========================================================================
    
    LOG_INFO("[DATA Channel] Registering peer-to-peer handlers...");
    
    if (state->raft_state) {
        // Standard Raft consensus protocol RPCs:
        // - RequestVote (§5.2: Leader election)
        // - AppendEntries (§5.3: Log replication & heartbeats)
        // - InstallSnapshot (§7: Log compaction)
        
        if (raft_register_handlers(registry, state->raft_state) != 0) {
            LOG_ERROR("Failed to register Raft consensus handlers");
            rpc_handler_registry_destroy(registry);
            return NULL;
        }
        
        LOG_INFO("  ✓ Registered 3 Raft consensus handlers:");
        LOG_INFO("    - RequestVote (0x%02x)", FUNC_ID_RAFT_REQUEST_VOTE);
        LOG_INFO("    - AppendEntries (0x%02x)", FUNC_ID_RAFT_APPEND_ENTRIES);
        LOG_INFO("    - InstallSnapshot (0x%02x)", FUNC_ID_RAFT_INSTALL_SNAPSHOT);
    } else {
        LOG_WARN("Raft state not initialized - no consensus handlers");
    }
    
    // ========================================================================
    // CLIENT-FACING HANDLERS (INGRESS channel - only if has_ingress)
    // These are APPLICATION-LEVEL APIs for clients
    // ========================================================================
    
    if (caps->has_ingress) {
        LOG_INFO("[INGRESS Channel] Registering client-facing handlers...");
        
        if (!state->raft_datastore) {
            LOG_ERROR("has_ingress=true but raft_datastore is NULL!");
            rpc_handler_registry_destroy(registry);
            return NULL;
        }
        
        // ====================================================================
        // Raft KV Operations (linearizable reads/writes)
        // ====================================================================
        
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
        
        LOG_INFO("  ✓ Registered 4 KV operation handlers:");
        LOG_INFO("    - SET (0x%02x)", FUNC_ID_RAFT_KV_SET);
        LOG_INFO("    - GET (0x%02x)", FUNC_ID_RAFT_KV_GET);
        LOG_INFO("    - UNSET (0x%02x)", FUNC_ID_RAFT_KV_UNSET);
        LOG_INFO("    - LIST (0x%02x)", FUNC_ID_RAFT_KV_LIST);
        
        // ====================================================================
        // Cluster Introspection API (NOT standard Raft, but useful)
        // ====================================================================
        
        if (rpc_handler_register(registry, FUNC_ID_RAFT_STATUS,
                                handle_raft_status, state) != 0) {
            LOG_ERROR("Failed to register RAFT_STATUS handler");
            rpc_handler_registry_destroy(registry);
            return NULL;
        }
        
        LOG_INFO("  ✓ Registered 1 introspection handler:");
        LOG_INFO("    - STATUS (0x%02x) - Cluster health & leader info",
                 FUNC_ID_RAFT_STATUS);
        
        LOG_INFO("Total INGRESS handlers: 5");
        
    } else {
        LOG_INFO("Skipping INGRESS handlers (no client-facing capability)");
    }
    
    // ========================================================================
    // Summary
    // ========================================================================
    
    size_t total_handlers = 3;  // Raft consensus (always present)
    if (caps->has_ingress) {
        total_handlers += 5;  // KV ops + status
    }
    
    LOG_INFO("========================================");
    LOG_INFO("Handler Registry Built Successfully");
    LOG_INFO("  DATA (peer-to-peer): 3 handlers");
    LOG_INFO("  INGRESS (client): %s", 
             caps->has_ingress ? "5 handlers" : "DISABLED");
    LOG_INFO("  Total: %zu handlers", total_handlers);
    LOG_INFO("========================================");
    
    return registry;
}

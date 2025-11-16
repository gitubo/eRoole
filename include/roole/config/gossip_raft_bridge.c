// include/roole/cluster/gossip_raft_bridge.h
// Clean separation: SWIM Gossip handles membership, Raft handles data

#ifndef ROOLE_GOSSIP_RAFT_BRIDGE_H
#define ROOLE_GOSSIP_RAFT_BRIDGE_H

#include "roole/cluster/cluster_types.h"
#include "roole/raft/raft_state.h"

/**
 * Gossip→Raft integration bridge
 * 
 * RESPONSIBILITIES:
 * ✅ Translate SWIM membership events into Raft peer operations
 * ✅ Filter out self-events
 * ✅ Handle peer discovery (JOIN)
 * ✅ Handle peer failures (FAILED, LEAVE)
 * ✅ Track peer health (SUSPECT)
 * 
 * NOT RESPONSIBLE FOR:
 * ❌ Data storage or replication (Raft's job)
 * ❌ Conflict resolution (Raft's job)
 * ❌ Consensus (Raft's job)
 */

typedef struct gossip_raft_bridge gossip_raft_bridge_t;

/**
 * Create gossip→raft bridge
 * @param my_id This node's ID (to filter self-events)
 * @param raft_state Raft state machine
 * @return Bridge handle
 */
gossip_raft_bridge_t* gossip_raft_bridge_create(node_id_t my_id,
                                                 raft_state_t *raft_state);

/**
 * Destroy bridge
 * @param bridge Bridge handle
 */
void gossip_raft_bridge_destroy(gossip_raft_bridge_t *bridge);

/**
 * Handle gossip membership event
 * Call this from membership event callback
 * 
 * @param bridge Bridge handle
 * @param node_id Peer node ID
 * @param type Peer type (ROUTER/WORKER)
 * @param ip Peer IP address
 * @param data_port Peer RPC data port
 * @param event_type Event type (JOIN/LEAVE/FAILED/UPDATE)
 */
void gossip_raft_bridge_handle_event(gossip_raft_bridge_t *bridge,
                                     node_id_t node_id,
                                     node_type_t type,
                                     const char *ip,
                                     uint16_t data_port,
                                     const char *event_type);

/**
 * Get statistics
 */
typedef struct {
    uint64_t peers_added;       // Peers added to Raft
    uint64_t peers_removed;     // Peers removed from Raft
    uint64_t events_filtered;   // Self-events filtered out
    uint64_t events_ignored;    // Unknown event types
} gossip_raft_bridge_stats_t;

void gossip_raft_bridge_get_stats(gossip_raft_bridge_t *bridge,
                                  gossip_raft_bridge_stats_t *out_stats);

#endif // ROOLE_GOSSIP_RAFT_BRIDGE_H

// ============================================================================
// IMPLEMENTATION
// ============================================================================

// src/cluster/gossip_raft_bridge.c

#define _POSIX_C_SOURCE 200809L

#include "roole/cluster/gossip_raft_bridge.h"
#include "roole/logger/logger.h"
#include "roole/core/common.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

struct gossip_raft_bridge {
    node_id_t my_id;
    raft_state_t *raft_state;
    
    // Statistics
    gossip_raft_bridge_stats_t stats;
    pthread_mutex_t stats_lock;
};

// ============================================================================
// LIFECYCLE
// ============================================================================

gossip_raft_bridge_t* gossip_raft_bridge_create(node_id_t my_id,
                                                 raft_state_t *raft_state) {
    if (!raft_state) {
        LOG_ERROR("Cannot create bridge: NULL raft_state");
        return NULL;
    }
    
    gossip_raft_bridge_t *bridge = safe_calloc(1, sizeof(gossip_raft_bridge_t));
    if (!bridge) {
        LOG_ERROR("Failed to allocate gossip→raft bridge");
        return NULL;
    }
    
    bridge->my_id = my_id;
    bridge->raft_state = raft_state;
    
    pthread_mutex_init(&bridge->stats_lock, NULL);
    
    LOG_INFO("Gossip→Raft bridge created (filtering self-events for node %u)", my_id);
    LOG_INFO("  Gossip Role: Membership discovery & failure detection");
    LOG_INFO("  Raft Role: Data storage & consensus");
    
    return bridge;
}

void gossip_raft_bridge_destroy(gossip_raft_bridge_t *bridge) {
    if (!bridge) return;
    
    pthread_mutex_destroy(&bridge->stats_lock);
    safe_free(bridge);
    
    LOG_INFO("Gossip→Raft bridge destroyed");
}

// ============================================================================
// EVENT HANDLING
// ============================================================================

void gossip_raft_bridge_handle_event(gossip_raft_bridge_t *bridge,
                                     node_id_t node_id,
                                     node_type_t type,
                                     const char *ip,
                                     uint16_t data_port,
                                     const char *event_type) {
    if (!bridge || !event_type) {
        LOG_ERROR("Invalid bridge event parameters");
        return;
    }
    
    // Filter out self-events
    if (node_id == bridge->my_id) {
        pthread_mutex_lock(&bridge->stats_lock);
        bridge->stats.events_filtered++;
        pthread_mutex_unlock(&bridge->stats_lock);
        return;
    }
    
    const char *type_str = (type == NODE_TYPE_ROUTER) ? "ROUTER" : "WORKER";
    
    LOG_DEBUG("Gossip→Raft bridge: node=%u type=%s event=%s", 
              node_id, type_str, event_type);
    
    // ========================================================================
    // PEER JOIN: Add to Raft cluster
    // ========================================================================
    
    if (strcmp(event_type, MEMBER_EVENT_JOIN) == 0) {
        LOG_INFO("🔗 Gossip discovered peer %u (%s:%u) → Adding to Raft cluster",
                 node_id, ip, data_port);
        
        int result = raft_add_peer(bridge->raft_state, node_id, ip, data_port);
        
        if (result == 0) {
            LOG_INFO("✓ Peer %u successfully added to Raft cluster", node_id);
            
            pthread_mutex_lock(&bridge->stats_lock);
            bridge->stats.peers_added++;
            pthread_mutex_unlock(&bridge->stats_lock);
        } else {
            LOG_WARN("⚠ Failed to add peer %u to Raft (may already exist)", node_id);
        }
        
        return;
    }
    
    // ========================================================================
    // PEER FAILED/LEFT: Remove from Raft cluster
    // ========================================================================
    
    if (strcmp(event_type, MEMBER_EVENT_FAILED) == 0 ||
        strcmp(event_type, MEMBER_EVENT_LEAVE) == 0) {
        
        const char *reason = strcmp(event_type, MEMBER_EVENT_FAILED) == 0 ? 
                            "FAILED" : "LEFT";
        
        LOG_INFO("💔 Gossip detected peer %u %s → Removing from Raft cluster",
                 node_id, reason);
        
        int result = raft_remove_peer(bridge->raft_state, node_id);
        
        if (result == 0) {
            LOG_INFO("✓ Peer %u successfully removed from Raft cluster", node_id);
            
            pthread_mutex_lock(&bridge->stats_lock);
            bridge->stats.peers_removed++;
            pthread_mutex_unlock(&bridge->stats_lock);
        } else {
            LOG_WARN("⚠ Peer %u not found in Raft cluster", node_id);
        }
        
        return;
    }
    
    // ========================================================================
    // PEER SUSPECT: Log but don't act (Raft has its own health checks)
    // ========================================================================
    
    if (strcmp(event_type, MEMBER_EVENT_UPDATE) == 0) {
        LOG_DEBUG("ℹ️ Gossip suspects peer %u (Raft managing its own health)", 
                  node_id);
        // No action needed - Raft's election timeout will handle it
        return;
    }
    
    // ========================================================================
    // Unknown event type
    // ========================================================================
    
    LOG_WARN("Unknown gossip event type: %s (ignoring)", event_type);
    
    pthread_mutex_lock(&bridge->stats_lock);
    bridge->stats.events_ignored++;
    pthread_mutex_unlock(&bridge->stats_lock);
}

// ============================================================================
// STATISTICS
// ============================================================================

void gossip_raft_bridge_get_stats(gossip_raft_bridge_t *bridge,
                                  gossip_raft_bridge_stats_t *out_stats) {
    if (!bridge || !out_stats) return;
    
    pthread_mutex_lock(&bridge->stats_lock);
    *out_stats = bridge->stats;
    pthread_mutex_unlock(&bridge->stats_lock);
}

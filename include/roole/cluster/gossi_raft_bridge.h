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
// include/roole/gossip/gossip_protocol.h
// SWIM protocol state machine (pure logic, no I/O)

#ifndef ROOLE_GOSSIP_PROTOCOL_H
#define ROOLE_GOSSIP_PROTOCOL_H

#include "roole/gossip/gossip_types.h"
#include "roole/cluster/cluster_view.h"

// Protocol callbacks (invoked by state machine)
typedef struct {
    // Membership changes
    void (*on_member_alive)(node_id_t node_id, const gossip_member_update_t *update, void *ctx);
    void (*on_member_suspect)(node_id_t node_id, uint64_t incarnation, void *ctx);
    void (*on_member_dead)(node_id_t node_id, void *ctx);
    
    // Protocol wants to send message (implementation sends via transport)
    void (*on_send_message)(const gossip_message_t *msg, const char *dest_ip, 
                           uint16_t dest_port, void *ctx);
} gossip_protocol_callbacks_t;

typedef struct {
    uint64_t pings_sent;
    uint64_t acks_received;
    uint64_t ack_timeouts;
    uint64_t suspect_count;
    uint64_t dead_count;
    uint64_t updates_sent;
    uint64_t updates_received;
} gossip_protocol_stats_t;

#define MAX_PENDING_ACKS 64

typedef struct pending_ack {
    node_id_t target_node;
    uint64_t ping_sent_ms;
    int active;
} pending_ack_t;

typedef struct {
    node_id_t my_id;
    node_type_t my_type;
    char my_ip[MAX_IP_LEN];
    uint16_t gossip_port;
    uint16_t data_port;
    uint64_t incarnation;
    uint64_t sequence_num;
    
    gossip_config_t config;
    cluster_view_t *cluster_view;
    
    pending_ack_t pending_acks[MAX_PENDING_ACKS];
    
    gossip_protocol_callbacks_t callbacks;
    void *callback_context;
    
    gossip_protocol_stats_t stats;
} gossip_protocol_t;

/**
 * Create protocol instance
 * @param my_id This node's ID
 * @param my_type This node's type
 * @param my_ip This node's IP address
 * @param gossip_port This node's gossip port
 * @param data_port This node's data port
 * @param config Protocol configuration
 * @param cluster_view Shared cluster view (updated by protocol)
 * @param callbacks Protocol callbacks
 * @param callback_context Context passed to callbacks
 * @return Protocol handle, or NULL on error
 */
gossip_protocol_t* gossip_protocol_create(
    node_id_t my_id,
    node_type_t my_type,
    const char *my_ip,
    uint16_t gossip_port,
    uint16_t data_port,
    const gossip_config_t *config,
    cluster_view_t *cluster_view,
    const gossip_protocol_callbacks_t *callbacks,
    void *callback_context
);

/**
 * Handle incoming gossip message
 * Pure state machine - updates cluster_view, invokes callbacks
 * @param proto Protocol handle
 * @param msg Received message
 * @param src_ip Source IP address
 * @param src_port Source port
 * @return 0 on success, -1 on error
 */
int gossip_protocol_handle_message(
    gossip_protocol_t *proto,
    const gossip_message_t *msg,
    const char *src_ip,
    uint16_t src_port
);

/**
 * Run one SWIM protocol round
 * - Selects random peer
 * - Sends PING with piggybacked updates
 * - Tracks pending ACK
 * @param proto Protocol handle
 */
void gossip_protocol_run_swim_round(gossip_protocol_t *proto);

/**
 * Check for timeouts
 * - ACK timeouts → mark SUSPECT
 * - SUSPECT timeouts → mark DEAD
 * @param proto Protocol handle
 */
void gossip_protocol_check_timeouts(gossip_protocol_t *proto);

/**
 * Announce join to cluster
 * Adds self to cluster view, queues JOIN update
 * @param proto Protocol handle
 */
void gossip_protocol_announce_join(gossip_protocol_t *proto);

/**
 * Announce graceful leave
 * Marks self as DEAD, queues LEAVE update
 * @param proto Protocol handle
 */
void gossip_protocol_announce_leave(gossip_protocol_t *proto);

/**
 * Add seed node for bootstrap
 * Queues JOIN message to be sent to seed
 * @param proto Protocol handle
 * @param seed_ip Seed node IP
 * @param seed_port Seed node gossip port
 */
void gossip_protocol_add_seed(gossip_protocol_t *proto, const char *seed_ip, uint16_t seed_port);

/**
 * Get protocol statistics
 * @param proto Protocol handle
 * @param out_stats Output statistics
 */
void gossip_protocol_get_stats(gossip_protocol_t *proto, gossip_protocol_stats_t *out_stats);

/**
 * Destroy protocol
 * @param proto Protocol handle
 */
void gossip_protocol_destroy(gossip_protocol_t *proto);

int add_pending_ack(gossip_protocol_t *proto, node_id_t target_node);

int remove_pending_ack(gossip_protocol_t *proto, node_id_t target_node);



#endif // ROOLE_GOSSIP_PROTOCOL_H
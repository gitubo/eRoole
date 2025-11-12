// src/gossip/protocol/swim_state.c
// Pure SWIM protocol state machine - NO I/O, only state transitions

#define _POSIX_C_SOURCE 200809L

#include "roole/gossip/gossip_protocol.h"
#include "roole/core/logger.h"
#include <stdlib.h>
#include <string.h>

#define MAX_PENDING_ACKS 64

typedef struct pending_ack {
    node_id_t target_node;
    uint64_t ping_sent_ms;
    int active;
} pending_ack_t;

struct gossip_protocol {
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
};

// ============================================================================
// PENDING ACK MANAGEMENT
// ============================================================================

static int add_pending_ack(gossip_protocol_t *proto, node_id_t target_node) {
    for (int i = 0; i < MAX_PENDING_ACKS; i++) {
        if (!proto->pending_acks[i].active) {
            proto->pending_acks[i].target_node = target_node;
            proto->pending_acks[i].ping_sent_ms = time_now_ms();
            proto->pending_acks[i].active = 1;
            return 0;
        }
    }
    LOG_WARN("Pending ACK table full");
    return -1;
}

static int remove_pending_ack(gossip_protocol_t *proto, node_id_t target_node) {
    for (int i = 0; i < MAX_PENDING_ACKS; i++) {
        if (proto->pending_acks[i].active && 
            proto->pending_acks[i].target_node == target_node) {
            proto->pending_acks[i].active = 0;
            return 0;
        }
    }
    return -1;
}

// ============================================================================
// PROTOCOL CREATION
// ============================================================================

gossip_protocol_t* gossip_protocol_create(
    node_id_t my_id,
    node_type_t my_type,
    const char *my_ip,
    uint16_t gossip_port,
    uint16_t data_port,
    const gossip_config_t *config,
    cluster_view_t *cluster_view,
    const gossip_protocol_callbacks_t *callbacks,
    void *callback_context)
{
    if (!my_ip || !cluster_view || !callbacks) {
        LOG_ERROR("Invalid parameters for gossip protocol");
        return NULL;
    }
    
    gossip_protocol_t *proto = calloc(1, sizeof(gossip_protocol_t));
    if (!proto) {
        LOG_ERROR("Failed to allocate gossip protocol");
        return NULL;
    }
    
    proto->my_id = my_id;
    proto->my_type = my_type;
    safe_strncpy(proto->my_ip, my_ip, MAX_IP_LEN);
    proto->gossip_port = gossip_port;
    proto->data_port = data_port;
    proto->incarnation = 0;
    proto->sequence_num = 0;
    proto->cluster_view = cluster_view;
    proto->callbacks = *callbacks;
    proto->callback_context = callback_context;
    
    if (config) {
        proto->config = *config;
    } else {
        proto->config = gossip_default_config();
    }
    
    memset(&proto->stats, 0, sizeof(proto->stats));
    
    LOG_INFO("SWIM protocol created (node_id=%u, type=%d)", my_id, my_type);
    return proto;
}

void gossip_protocol_run_swim_round(gossip_protocol_t *proto)
{
    if (!proto) return;
    
    // Select random peer (exclude self and dead nodes)
    node_id_t target = 0;
    node_id_t peer_list[MAX_CLUSTER_NODES];
    size_t peer_count = 0;
    
    pthread_rwlock_rdlock(&proto->cluster_view->lock);
    
    for (size_t i = 0; i < proto->cluster_view->count; i++) {
        cluster_member_t *m = &proto->cluster_view->members[i];
        
        if (m->node_id != proto->my_id && m->status != NODE_STATUS_DEAD) {
            peer_list[peer_count++] = m->node_id;
        }
    }
    
    pthread_rwlock_unlock(&proto->cluster_view->lock);
    
    if (peer_count == 0) {
        LOG_DEBUG("SWIM: No peers available for PING");
        return;
    }
    
    target = peer_list[rand() % peer_count];
    
    // Get target info
    cluster_member_t *target_member = cluster_view_get(proto->cluster_view, target);
    if (!target_member) return;
    
    char target_ip[MAX_IP_LEN];
    uint16_t target_port = target_member->gossip_port;
    safe_strncpy(target_ip, target_member->ip_address, MAX_IP_LEN);
    
    cluster_view_release(proto->cluster_view);
    
    // Build PING message with cluster state
    gossip_message_t ping_msg = {
        .version = 1,
        .msg_type = GOSSIP_MSG_PING,
        .sender_id = proto->my_id,
        .sequence_num = __sync_fetch_and_add(&proto->sequence_num, 1),
        .num_updates = 0
    };
    
    // Include cluster state (anti-entropy)
    pthread_rwlock_rdlock(&proto->cluster_view->lock);
    
    for (size_t i = 0; i < proto->cluster_view->count && 
         ping_msg.num_updates < GOSSIP_MAX_PIGGYBACK_UPDATES; i++) {
        
        cluster_member_t *m = &proto->cluster_view->members[i];
        
        if (m->status == NODE_STATUS_DEAD || m->node_id == proto->my_id) {
            continue;
        }
        
        gossip_member_update_t *upd = &ping_msg.updates[ping_msg.num_updates];
        upd->node_id = m->node_id;
        upd->node_type = m->node_type;
        safe_strncpy(upd->ip_address, m->ip_address, MAX_IP_LEN);
        upd->gossip_port = m->gossip_port;
        upd->data_port = m->data_port;
        upd->status = m->status;
        upd->incarnation = m->incarnation;
        upd->timestamp_ms = time_now_ms();
        
        ping_msg.num_updates++;
    }
    
    pthread_rwlock_unlock(&proto->cluster_view->lock);
    
    // Track pending ACK
    add_pending_ack(proto, target);
    proto->stats.pings_sent++;
    
    // Request engine to send PING
    if (proto->callbacks.on_send_message) {
        proto->callbacks.on_send_message(&ping_msg, target_ip, target_port,
                                        proto->callback_context);
    }
    
    LOG_DEBUG("SWIM: Sent PING to node %u (%s:%u, updates=%u)",
              target, target_ip, target_port, ping_msg.num_updates);
}

void gossip_protocol_check_timeouts(gossip_protocol_t *proto)
{
    if (!proto) return;
    
    uint64_t now = time_now_ms();
    
    // Check ACK timeouts
    for (int i = 0; i < MAX_PENDING_ACKS; i++) {
        if (!proto->pending_acks[i].active) continue;
        
        uint64_t elapsed = now - proto->pending_acks[i].ping_sent_ms;
        
        if (elapsed > proto->config.ack_timeout_ms) {
            node_id_t node = proto->pending_acks[i].target_node;
            
            LOG_WARN("SWIM: ACK timeout for node %u (%lums)", node, elapsed);
            proto->stats.ack_timeouts++;
            
            // Get current status
            cluster_member_t *member = cluster_view_get(proto->cluster_view, node);
            
            if (member && member->status == NODE_STATUS_ALIVE) {
                uint64_t incarnation = member->incarnation;
                cluster_view_release(proto->cluster_view);
                
                // Mark as SUSPECT
                cluster_view_update_status(proto->cluster_view, node,
                                         NODE_STATUS_SUSPECT, incarnation);
                
                LOG_INFO("SWIM: Node %u marked as SUSPECT (ACK timeout)", node);
                proto->stats.suspect_count++;
                
                if (proto->callbacks.on_member_suspect) {
                    proto->callbacks.on_member_suspect(node, incarnation,
                                                      proto->callback_context);
                }
            } else if (member) {
                cluster_view_release(proto->cluster_view);
            }
            
            proto->pending_acks[i].active = 0;
        }
    }
    
    // Check SUSPECT -> DEAD timeouts
    pthread_rwlock_rdlock(&proto->cluster_view->lock);
    
    for (size_t i = 0; i < proto->cluster_view->count; i++) {
        cluster_member_t *m = &proto->cluster_view->members[i];
        
        if (m->status != NODE_STATUS_SUSPECT) continue;
        
        uint64_t elapsed = now - m->last_seen_ms;
        
        if (elapsed > proto->config.dead_timeout_ms) {
            node_id_t node_id = m->node_id;
            uint64_t incarnation = m->incarnation;
            
            pthread_rwlock_unlock(&proto->cluster_view->lock);
            
            LOG_ERROR("SWIM: Node %u suspected for %lums, marking as DEAD",
                     node_id, elapsed);
            
            cluster_view_update_status(proto->cluster_view, node_id,
                                     NODE_STATUS_DEAD, incarnation);
            
            proto->stats.dead_count++;
            
            if (proto->callbacks.on_member_dead) {
                proto->callbacks.on_member_dead(node_id, proto->callback_context);
            }
            
            pthread_rwlock_rdlock(&proto->cluster_view->lock);
        }
    }
    
    pthread_rwlock_unlock(&proto->cluster_view->lock);
}

void gossip_protocol_announce_join(gossip_protocol_t *proto)
{
    if (!proto) return;
    
    LOG_INFO("SWIM: Announcing join to cluster");
    
    // Add self to cluster view
    cluster_member_t self = {
        .node_id = proto->my_id,
        .node_type = proto->my_type,
        .gossip_port = proto->gossip_port,
        .data_port = proto->data_port,
        .status = NODE_STATUS_ALIVE,
        .incarnation = proto->incarnation,
        .last_seen_ms = time_now_ms()
    };
    safe_strncpy(self.ip_address, proto->my_ip, MAX_IP_LEN);
    
    cluster_view_add(proto->cluster_view, &self);
}

void gossip_protocol_announce_leave(gossip_protocol_t *proto)
{
    if (!proto) return;
    
    LOG_INFO("SWIM: Announcing graceful leave");
    
    gossip_message_t leave_msg = {
        .version = 1,
        .msg_type = GOSSIP_MSG_LEAVE,
        .sender_id = proto->my_id,
        .sequence_num = __sync_fetch_and_add(&proto->sequence_num, 1),
        .num_updates = 1
    };
    
    leave_msg.updates[0].node_id = proto->my_id;
    leave_msg.updates[0].status = NODE_STATUS_DEAD;
    leave_msg.updates[0].incarnation = proto->incarnation;
    leave_msg.updates[0].timestamp_ms = time_now_ms();
    
    // Request engine to broadcast LEAVE
    if (proto->callbacks.on_send_message) {
        proto->callbacks.on_send_message(&leave_msg, NULL, 0,
                                        proto->callback_context);
    }
}

void gossip_protocol_add_seed(gossip_protocol_t *proto,
                             const char *seed_ip,
                             uint16_t seed_port)
{
    if (!proto || !seed_ip) return;
    
    LOG_INFO("SWIM: Adding seed node %s:%u", seed_ip, seed_port);
    
    gossip_message_t join_msg = {
        .version = 1,
        .msg_type = GOSSIP_MSG_JOIN,
        .sender_id = proto->my_id,
        .sequence_num = __sync_fetch_and_add(&proto->sequence_num, 1),
        .num_updates = 1
    };
    
    join_msg.updates[0].node_id = proto->my_id;
    join_msg.updates[0].node_type = proto->my_type;
    join_msg.updates[0].status = NODE_STATUS_ALIVE;
    join_msg.updates[0].incarnation = proto->incarnation;
    join_msg.updates[0].gossip_port = proto->gossip_port;
    join_msg.updates[0].data_port = proto->data_port;
    join_msg.updates[0].timestamp_ms = time_now_ms();
    safe_strncpy(join_msg.updates[0].ip_address, proto->my_ip, MAX_IP_LEN);
    
    // Request engine to send JOIN to seed
    if (proto->callbacks.on_send_message) {
        proto->callbacks.on_send_message(&join_msg, seed_ip, seed_port,
                                        proto->callback_context);
    }
}

void gossip_protocol_get_stats(gossip_protocol_t *proto,
                              gossip_protocol_stats_t *out_stats)
{
    if (!proto || !out_stats) return;
    *out_stats = proto->stats;
}

void gossip_protocol_destroy(gossip_protocol_t *proto)
{
    if (!proto) return;
    
    LOG_INFO("SWIM protocol destroyed");
    free(proto);
}
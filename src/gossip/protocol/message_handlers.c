#include "roole/gossip/gossip_protocol.h"
#include "roole/core/common.h"
#include "roole/logger/logger.h"


// ============================================================================
// MESSAGE HANDLERS (Pure state transitions)
// ============================================================================

static void handle_ping(gossip_protocol_t *proto,
                       const gossip_message_t *msg,
                       const char *src_ip,
                       uint16_t src_port)
{
    LOG_DEBUG("SWIM: Processing PING from node %u (updates=%u)", 
              msg->sender_id, msg->num_updates);
    
    // Process piggyback updates
    for (uint8_t i = 0; i < msg->num_updates; i++) {
        const gossip_member_update_t *upd = &msg->updates[i];
        
        cluster_member_t *existing = cluster_view_get(proto->cluster_view, upd->node_id);
        
        if (!existing) {
            // New member discovered
            cluster_member_t new_member = {
                .node_id = upd->node_id,
                .node_type = upd->node_type,
                .gossip_port = upd->gossip_port,
                .data_port = upd->data_port,
                .status = upd->status,
                .incarnation = upd->incarnation,
                .last_seen_ms = time_now_ms()
            };
            safe_strncpy(new_member.ip_address, upd->ip_address, MAX_IP_LEN);
            
            cluster_view_add(proto->cluster_view, &new_member);
            
            LOG_INFO("SWIM: Discovered new member %u via PING", upd->node_id);
            proto->stats.updates_received++;
            
            // Notify callback
            if (proto->callbacks.on_member_alive) {
                proto->callbacks.on_member_alive(upd->node_id, upd, 
                                                proto->callback_context);
            }
        } else {
            // Handle rejoin (DEAD -> ALIVE with higher incarnation)
            if (existing->status == NODE_STATUS_DEAD && 
                upd->status == NODE_STATUS_ALIVE &&
                upd->incarnation > existing->incarnation) {
                
                cluster_view_release(proto->cluster_view);
                
                cluster_member_t rejoin = {
                    .node_id = upd->node_id,
                    .node_type = upd->node_type,
                    .gossip_port = upd->gossip_port,
                    .data_port = upd->data_port,
                    .status = NODE_STATUS_ALIVE,
                    .incarnation = upd->incarnation,
                    .last_seen_ms = time_now_ms()
                };
                safe_strncpy(rejoin.ip_address, upd->ip_address, MAX_IP_LEN);
                
                cluster_view_add(proto->cluster_view, &rejoin);
                
                LOG_INFO("SWIM: Node %u rejoined (inc=%lu)", upd->node_id, upd->incarnation);
                
                if (proto->callbacks.on_member_alive) {
                    proto->callbacks.on_member_alive(upd->node_id, upd,
                                                    proto->callback_context);
                }
            } else if (upd->incarnation > existing->incarnation) {
                // Standard update
                node_status_t old_status = existing->status;
                cluster_view_release(proto->cluster_view);
                
                cluster_view_update_status(proto->cluster_view, upd->node_id,
                                         upd->status, upd->incarnation);
                
                proto->stats.updates_received++;
                
                // Trigger appropriate callback
                if (upd->status == NODE_STATUS_SUSPECT && old_status == NODE_STATUS_ALIVE) {
                    proto->stats.suspect_count++;
                    if (proto->callbacks.on_member_suspect) {
                        proto->callbacks.on_member_suspect(upd->node_id, 
                                                          upd->incarnation,
                                                          proto->callback_context);
                    }
                } else if (upd->status == NODE_STATUS_DEAD) {
                    proto->stats.dead_count++;
                    if (proto->callbacks.on_member_dead) {
                        proto->callbacks.on_member_dead(upd->node_id,
                                                       proto->callback_context);
                    }
                } else if (upd->status == NODE_STATUS_ALIVE) {
                    if (proto->callbacks.on_member_alive) {
                        proto->callbacks.on_member_alive(upd->node_id, upd,
                                                        proto->callback_context);
                    }
                }
            } else {
                cluster_view_release(proto->cluster_view);
            }
        }
    }
    
    // Build ACK message (protocol decides what to send)
    gossip_message_t ack_msg = {
        .version = 1,
        .msg_type = GOSSIP_MSG_ACK,
        .flags = 0,
        .sender_id = proto->my_id,
        .sequence_num = __sync_fetch_and_add(&proto->sequence_num, 1),
        .num_updates = 0
    };
    
    // Include cluster state in ACK (anti-entropy)
    pthread_rwlock_rdlock(&proto->cluster_view->lock);
    
    size_t max_updates = ROOLE_MIN(proto->cluster_view->count,
                                   GOSSIP_MAX_PIGGYBACK_UPDATES);
    
    for (size_t i = 0; i < max_updates && ack_msg.num_updates < GOSSIP_MAX_PIGGYBACK_UPDATES; i++) {
        cluster_member_t *m = &proto->cluster_view->members[i];
        
        if (m->status == NODE_STATUS_DEAD || m->node_id == proto->my_id) {
            continue;
        }
        
        gossip_member_update_t *upd = &ack_msg.updates[ack_msg.num_updates];
        upd->node_id = m->node_id;
        upd->node_type = m->node_type;
        safe_strncpy(upd->ip_address, m->ip_address, MAX_IP_LEN);
        upd->gossip_port = m->gossip_port;
        upd->data_port = m->data_port;
        upd->status = m->status;
        upd->incarnation = m->incarnation;
        upd->timestamp_ms = time_now_ms();
        
        ack_msg.num_updates++;
    }
    
    pthread_rwlock_unlock(&proto->cluster_view->lock);
    
    // Request engine to send ACK
    if (proto->callbacks.on_send_message) {
        proto->callbacks.on_send_message(&ack_msg, src_ip, src_port,
                                        proto->callback_context);
    }
}

static void handle_ack(gossip_protocol_t *proto,
                      const gossip_message_t *msg,
                      const char *src_ip,
                      uint16_t src_port)
{
    (void)src_ip;
    (void)src_port;
    
    LOG_DEBUG("SWIM: Processing ACK from node %u (updates=%u)",
              msg->sender_id, msg->num_updates);
    
    // Clear pending ACK
    remove_pending_ack(proto, msg->sender_id);
    proto->stats.acks_received++;
    
    // Check if sender was suspected - if so, mark alive
    cluster_member_t *member = cluster_view_get(proto->cluster_view, msg->sender_id);
    if (member && member->status == NODE_STATUS_SUSPECT) {
        uint64_t incarnation = member->incarnation;
        cluster_view_release(proto->cluster_view);
        
        cluster_view_update_status(proto->cluster_view, msg->sender_id,
                                  NODE_STATUS_ALIVE, incarnation);
        
        LOG_INFO("SWIM: Node %u recovered from SUSPECT", msg->sender_id);
    } else if (member) {
        cluster_view_release(proto->cluster_view);
    }
    
    // Process piggyback updates (same as PING)
    for (uint8_t i = 0; i < msg->num_updates; i++) {
        const gossip_member_update_t *upd = &msg->updates[i];
        
        cluster_member_t *existing = cluster_view_get(proto->cluster_view, upd->node_id);
        
        if (!existing) {
            // New member
            cluster_member_t new_member = {
                .node_id = upd->node_id,
                .node_type = upd->node_type,
                .gossip_port = upd->gossip_port,
                .data_port = upd->data_port,
                .status = upd->status,
                .incarnation = upd->incarnation,
                .last_seen_ms = time_now_ms()
            };
            safe_strncpy(new_member.ip_address, upd->ip_address, MAX_IP_LEN);
            
            cluster_view_add(proto->cluster_view, &new_member);
            
            LOG_INFO("SWIM: Discovered new member %u via ACK", upd->node_id);
            proto->stats.updates_received++;
            
            if (proto->callbacks.on_member_alive) {
                proto->callbacks.on_member_alive(upd->node_id, upd,
                                                proto->callback_context);
            }
        } else if (upd->incarnation > existing->incarnation) {
            cluster_view_release(proto->cluster_view);
            
            cluster_view_update_status(proto->cluster_view, upd->node_id,
                                     upd->status, upd->incarnation);
            
            proto->stats.updates_received++;
        } else {
            cluster_view_release(proto->cluster_view);
        }
    }
}

static void handle_suspect(gossip_protocol_t *proto,
                          const gossip_message_t *msg)
{
    LOG_DEBUG("SWIM: Processing SUSPECT from node %u", msg->sender_id);
    
    for (uint8_t i = 0; i < msg->num_updates; i++) {
        const gossip_member_update_t *upd = &msg->updates[i];
        
        if (upd->node_id == proto->my_id && upd->status == NODE_STATUS_SUSPECT) {
            // Someone suspects US - refute with higher incarnation
            proto->incarnation++;
            
            LOG_WARN("SWIM: Refuting suspicion (inc=%lu)", proto->incarnation);
            
            gossip_member_update_t alive_update = {
                .node_id = proto->my_id,
                .node_type = proto->my_type,
                .status = NODE_STATUS_ALIVE,
                .incarnation = proto->incarnation,
                .gossip_port = proto->gossip_port,
                .data_port = proto->data_port,
                .timestamp_ms = time_now_ms()
            };
            safe_strncpy(alive_update.ip_address, proto->my_ip, MAX_IP_LEN);
            
            // Broadcast ALIVE message
            gossip_message_t alive_msg = {
                .version = 1,
                .msg_type = GOSSIP_MSG_ALIVE,
                .sender_id = proto->my_id,
                .sequence_num = __sync_fetch_and_add(&proto->sequence_num, 1),
                .num_updates = 1
            };
            alive_msg.updates[0] = alive_update;
            
            // Send to all peers (engine will handle broadcast)
            if (proto->callbacks.on_send_message) {
                proto->callbacks.on_send_message(&alive_msg, NULL, 0,
                                                proto->callback_context);
            }
        } else if (upd->node_id != proto->my_id && upd->status == NODE_STATUS_SUSPECT) {
            // Mark other node as suspect
            cluster_member_t *existing = cluster_view_get(proto->cluster_view, upd->node_id);
            
            if (existing && upd->incarnation >= existing->incarnation &&
                existing->status == NODE_STATUS_ALIVE) {
                cluster_view_release(proto->cluster_view);
                
                cluster_view_update_status(proto->cluster_view, upd->node_id,
                                         NODE_STATUS_SUSPECT, upd->incarnation);
                
                LOG_INFO("SWIM: Marking node %u as SUSPECT", upd->node_id);
                proto->stats.suspect_count++;
                
                if (proto->callbacks.on_member_suspect) {
                    proto->callbacks.on_member_suspect(upd->node_id, upd->incarnation,
                                                      proto->callback_context);
                }
            } else if (existing) {
                cluster_view_release(proto->cluster_view);
            }
        }
    }
}

static void handle_alive(gossip_protocol_t *proto,
                        const gossip_message_t *msg)
{
    LOG_DEBUG("SWIM: Processing ALIVE from node %u", msg->sender_id);
    
    for (uint8_t i = 0; i < msg->num_updates; i++) {
        const gossip_member_update_t *upd = &msg->updates[i];
        
        cluster_member_t *existing = cluster_view_get(proto->cluster_view, upd->node_id);
        
        if (existing && upd->incarnation > existing->incarnation) {
            cluster_view_release(proto->cluster_view);
            
            cluster_view_update_status(proto->cluster_view, upd->node_id,
                                     NODE_STATUS_ALIVE, upd->incarnation);
            
            LOG_INFO("SWIM: Node %u refuted suspicion (inc=%lu)",
                     upd->node_id, upd->incarnation);
            
            if (proto->callbacks.on_member_alive) {
                proto->callbacks.on_member_alive(upd->node_id, upd,
                                                proto->callback_context);
            }
        } else if (existing) {
            cluster_view_release(proto->cluster_view);
        }
    }
}

static void handle_dead(gossip_protocol_t *proto,
                       const gossip_message_t *msg)
{
    LOG_DEBUG("SWIM: Processing DEAD from node %u", msg->sender_id);
    
    for (uint8_t i = 0; i < msg->num_updates; i++) {
        const gossip_member_update_t *upd = &msg->updates[i];
        
        cluster_member_t *existing = cluster_view_get(proto->cluster_view, upd->node_id);
        
        if (existing) {
            cluster_view_release(proto->cluster_view);
            
            cluster_view_update_status(proto->cluster_view, upd->node_id,
                                     NODE_STATUS_DEAD, upd->incarnation);
            
            LOG_INFO("SWIM: Node %u marked as DEAD", upd->node_id);
            proto->stats.dead_count++;
            
            if (proto->callbacks.on_member_dead) {
                proto->callbacks.on_member_dead(upd->node_id, proto->callback_context);
            }
        } else if (existing) {
            cluster_view_release(proto->cluster_view);
        }
    }
}

// ============================================================================
// PUBLIC API
// ============================================================================

int gossip_protocol_handle_message(
    gossip_protocol_t *proto,
    const gossip_message_t *msg,
    const char *src_ip,
    uint16_t src_port)
{
    if (!proto || !msg) return -1;
    
    // Ignore messages from self
    if (msg->sender_id == proto->my_id) {
        return 0;
    }
    
    switch (msg->msg_type) {
        case GOSSIP_MSG_PING:
            handle_ping(proto, msg, src_ip, src_port);
            break;
            
        case GOSSIP_MSG_ACK:
            handle_ack(proto, msg, src_ip, src_port);
            break;
            
        case GOSSIP_MSG_SUSPECT:
            handle_suspect(proto, msg);
            break;
            
        case GOSSIP_MSG_ALIVE:
            handle_alive(proto, msg);
            break;
            
        case GOSSIP_MSG_DEAD:
            handle_dead(proto, msg);
            break;

        case GOSSIP_MSG_JOIN:
        case GOSSIP_MSG_LEAVE:
            handle_ping(proto, msg, src_ip, src_port); 
            break;
        
        default:
            LOG_DEBUG("SWIM: Unhandled message type %u", msg->msg_type);
            break;
    }
    
    return 0;
}

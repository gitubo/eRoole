#include "roole/gossip/gossip_protocol.h"
#include "roole/core/common.h"
#include "roole/logger/logger.h"


// ============================================================================
// MESSAGE HANDLERS (Pure state transitions)
// ============================================================================

static void send_cluster_snapshot(gossip_protocol_t *proto,
                                  const char *dest_ip,
                                  uint16_t dest_port) {
    gossip_message_t response = {
        .version = 1,
        .msg_type = GOSSIP_MSG_JOIN_RESPONSE,  // ✅ Special message type
        .sender_id = proto->my_id,
        .sequence_num = __sync_fetch_and_add(&proto->sequence_num, 1),
        .num_updates = 0
    };
    
    pthread_rwlock_rdlock(&proto->cluster_view->lock);
    
    // Pack all alive members into response (including self)
    for (size_t i = 0; i < proto->cluster_view->count && 
         response.num_updates < GOSSIP_MAX_PIGGYBACK_UPDATES; i++) {
        
        cluster_member_t *m = &proto->cluster_view->members[i];
        
        // Include all members except DEAD ones
        if (m->status == NODE_STATUS_DEAD) continue;
        
        gossip_member_update_t *upd = &response.updates[response.num_updates];
        upd->node_id = m->node_id;
        upd->node_type = m->node_type;
        safe_strncpy(upd->ip_address, m->ip_address, MAX_IP_LEN);
        upd->gossip_port = m->gossip_port;
        upd->data_port = m->data_port;      // ✅ Critical for bootstrap!
        upd->status = m->status;
        upd->incarnation = m->incarnation;
        upd->timestamp_ms = time_now_ms();
        
        LOG_DEBUG("SWIM: Snapshot[%u]: node=%u type=%d data=%u",
                  response.num_updates, upd->node_id, upd->node_type, upd->data_port);
        
        response.num_updates++;
    }
    
    pthread_rwlock_unlock(&proto->cluster_view->lock);
    
    LOG_INFO("SWIM: Sending cluster snapshot to %s:%u (%u members)",
             dest_ip, dest_port, response.num_updates);
    
    if (proto->callbacks.on_send_message) {
        proto->callbacks.on_send_message(&response, dest_ip, dest_port,
                                        proto->callback_context);
    }
}

static void handle_ping(gossip_protocol_t *proto,
                       const gossip_message_t *msg,
                       const char *src_ip,
                       uint16_t src_port) {
    LOG_DEBUG("SWIM: Processing PING from node %u (updates=%u)", 
              msg->sender_id, msg->num_updates);
    
    // ✅ FIX: Don't add sender separately - they're in updates[0]!
    // The SWIM protocol design is that sender includes themselves
    // as the first update with complete metadata (type, ports, etc.)
    
    int sender_is_new = 0;  // Track if we discover new sender
    
    // Process ALL piggybacked updates (including sender in updates[0])
    for (uint8_t i = 0; i < msg->num_updates; i++) {
        const gossip_member_update_t *upd = &msg->updates[i];
        
        char actual_ip[MAX_IP_LEN];
        if (upd->node_id == msg->sender_id && 
            (strcmp(upd->ip_address, "0.0.0.0") == 0 || 
             strcmp(upd->ip_address, "") == 0)) {
            
            // Use UDP source IP instead
            safe_strncpy(actual_ip, src_ip, MAX_IP_LEN);
            LOG_INFO("SWIM: Corrected node %u IP from 0.0.0.0 to %s (from UDP source)",
                     upd->node_id, src_ip);
        } else {
            safe_strncpy(actual_ip, upd->ip_address, MAX_IP_LEN);
        }

        LOG_DEBUG("SWIM: Processing update[%u]: node=%u type=%d gossip=%u data=%u status=%d",
                  i, upd->node_id, upd->node_type, 
                  upd->gossip_port, upd->data_port, upd->status);
        
        cluster_member_t *existing = cluster_view_get(proto->cluster_view, upd->node_id);
        
        if (!existing) {
            // ✅ New member discovered (including sender if this is updates[0])
            cluster_member_t new_member = {
                .node_id = upd->node_id,
                .node_type = upd->node_type,      // ✅ Correct type
                .gossip_port = upd->gossip_port,
                .data_port = upd->data_port,      // ✅ Correct data port!
                .status = upd->status,
                .incarnation = upd->incarnation,
                .last_seen_ms = time_now_ms()
            };
            safe_strncpy(new_member.ip_address, actual_ip, MAX_IP_LEN);
            
            cluster_view_add(proto->cluster_view, &new_member);
            
            LOG_INFO("SWIM: Discovered new member %u (type=%d, gossip=%u, data=%u) via PING",
                     upd->node_id, upd->node_type, upd->gossip_port, upd->data_port);
            
            proto->stats.updates_received++;
            
            // Track if this is the sender
            if (upd->node_id == msg->sender_id) {
                sender_is_new = 1;
            }
            
            // Notify callback for new members
            if (proto->callbacks.on_member_alive) {
                gossip_member_update_t corrected = *upd;
                safe_strncpy(corrected.ip_address, actual_ip, MAX_IP_LEN);
                proto->callbacks.on_member_alive(upd->node_id, &corrected, 
                                                proto->callback_context);
            }
            
        } else {
            // ✅ Existing member - check for rejoin or status update
            cluster_view_release(proto->cluster_view);

            if (existing->status == NODE_STATUS_DEAD && 
                upd->status == NODE_STATUS_ALIVE &&
                upd->incarnation > existing->incarnation) {
                // Node rejoining after being dead
                cluster_view_release(proto->cluster_view);
                
                cluster_member_t rejoin = {
                    .node_id = upd->node_id,
                    .node_type = upd->node_type,
                    .gossip_port = upd->gossip_port,
                    .data_port = upd->data_port,      // ✅ Update data port on rejoin
                    .status = NODE_STATUS_ALIVE,
                    .incarnation = upd->incarnation,
                    .last_seen_ms = time_now_ms()
                };
                safe_strncpy(rejoin.ip_address, upd->ip_address, MAX_IP_LEN);
                
                cluster_view_add(proto->cluster_view, &rejoin);
                
                LOG_INFO("SWIM: Node %u rejoined (inc=%lu, data_port=%u)", 
                         upd->node_id, upd->incarnation, upd->data_port);
                
                if (proto->callbacks.on_member_alive) {
                    proto->callbacks.on_member_alive(upd->node_id, upd,
                                                    proto->callback_context);
                }
                
            } else if (upd->incarnation > existing->incarnation) {
                // Standard status update
                node_status_t old_status = existing->status;
                cluster_view_release(proto->cluster_view);
                
                cluster_view_update_status(proto->cluster_view, upd->node_id,
                                         upd->status, upd->incarnation);
                
                proto->stats.updates_received++;
                
                // Trigger appropriate callbacks
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
                // Stale update - ignore
                cluster_view_release(proto->cluster_view);
                LOG_DEBUG("SWIM: Ignoring stale update for node %u (inc %lu <= current)",
                         upd->node_id, upd->incarnation);
            }
        }
    }
    
    // ✅ Build ACK message with cluster state
    gossip_message_t ack_msg = {
        .version = 1,
        .msg_type = GOSSIP_MSG_ACK,
        .flags = 0,
        .sender_id = proto->my_id,
        .sequence_num = __sync_fetch_and_add(&proto->sequence_num, 1),
        .num_updates = 0
    };
    
    // Include our own info as first update
    gossip_member_update_t self_update = {
        .node_id = proto->my_id,
        .node_type = proto->my_type,
        .status = NODE_STATUS_ALIVE,
        .incarnation = proto->incarnation,
        .gossip_port = proto->gossip_port,
        .data_port = proto->data_port,      // ✅ Include our data port
        .timestamp_ms = time_now_ms()
    };
    safe_strncpy(self_update.ip_address, proto->my_ip, MAX_IP_LEN);
    
    ack_msg.updates[0] = self_update;
    ack_msg.num_updates = 1;
    
    // Include other cluster members (anti-entropy)
    pthread_rwlock_rdlock(&proto->cluster_view->lock);
    
    size_t max_updates = ROOLE_MIN(proto->cluster_view->count,
                                   GOSSIP_MAX_PIGGYBACK_UPDATES);
    
    for (size_t i = 0; i < max_updates && ack_msg.num_updates < GOSSIP_MAX_PIGGYBACK_UPDATES; i++) {
        cluster_member_t *m = &proto->cluster_view->members[i];
        
        if (m->status == NODE_STATUS_DEAD || m->node_id == proto->my_id) {
            continue;  // Skip dead members and self (already added)
        }
        
        gossip_member_update_t *upd = &ack_msg.updates[ack_msg.num_updates];
        upd->node_id = m->node_id;
        upd->node_type = m->node_type;
        safe_strncpy(upd->ip_address, m->ip_address, MAX_IP_LEN);
        upd->gossip_port = m->gossip_port;
        upd->data_port = m->data_port;      // ✅ Include data port
        upd->status = m->status;
        upd->incarnation = m->incarnation;
        upd->timestamp_ms = time_now_ms();
        
        ack_msg.num_updates++;
    }
    
    pthread_rwlock_unlock(&proto->cluster_view->lock);
    
    LOG_DEBUG("SWIM: Sending ACK to %s:%u with %u updates", 
              src_ip, src_port, ack_msg.num_updates);
    
    // Send ACK back to sender
    if (proto->callbacks.on_send_message) {
        proto->callbacks.on_send_message(&ack_msg, src_ip, src_port,
                                        proto->callback_context);
    }
    
    // ✅ If sender was new, send them full cluster snapshot
    if (sender_is_new) {
        LOG_INFO("SWIM: New member %u joining, sending cluster snapshot", msg->sender_id);
        send_cluster_snapshot(proto, src_ip, src_port);
    }
}

static void handle_ack(gossip_protocol_t *proto,
                      const gossip_message_t *msg,
                      const char *src_ip,
                      uint16_t src_port) {
    (void)src_ip;
    (void)src_port;
    
    LOG_DEBUG("SWIM: Processing ACK from node %u (updates=%u)",
              msg->sender_id, msg->num_updates);
    
    // Clear pending ACK (we received response)
    remove_pending_ack(proto, msg->sender_id);
    proto->stats.acks_received++;
    
    // ✅ Process ALL piggybacked updates (sender is in updates[0])
    // Don't special-case the sender - they're just another update!
    
    for (uint8_t i = 0; i < msg->num_updates; i++) {
        const gossip_member_update_t *upd = &msg->updates[i];
        
        LOG_DEBUG("SWIM: Processing ACK update[%u]: node=%u type=%d data_port=%u status=%d",
                  i, upd->node_id, upd->node_type, upd->data_port, upd->status);
        
        cluster_member_t *existing = cluster_view_get(proto->cluster_view, upd->node_id);
        
        if (!existing) {
            // ✅ New member discovered (including ACK sender if in updates[0])
            cluster_member_t new_member = {
                .node_id = upd->node_id,
                .node_type = upd->node_type,
                .gossip_port = upd->gossip_port,
                .data_port = upd->data_port,      // ✅ Correct data port
                .status = upd->status,
                .incarnation = upd->incarnation,
                .last_seen_ms = time_now_ms()
            };
            safe_strncpy(new_member.ip_address, upd->ip_address, MAX_IP_LEN);
            
            cluster_view_add(proto->cluster_view, &new_member);
            
            LOG_INFO("SWIM: Discovered new member %u (type=%d, data=%u) via ACK",
                     upd->node_id, upd->node_type, upd->data_port);
            
            proto->stats.updates_received++;
            
            // Notify callback
            if (proto->callbacks.on_member_alive) {
                proto->callbacks.on_member_alive(upd->node_id, upd,
                                                proto->callback_context);
            }
            
        } else {
            // ✅ Existing member - handle status updates
            
            // Special case: If sender was SUSPECT, mark them ALIVE (they responded!)
            if (upd->node_id == msg->sender_id && existing->status == NODE_STATUS_SUSPECT) {
                uint64_t incarnation = existing->incarnation;
                cluster_view_release(proto->cluster_view);
                
                cluster_view_update_status(proto->cluster_view, msg->sender_id,
                                         NODE_STATUS_ALIVE, incarnation);
                
                LOG_INFO("SWIM: Node %u recovered from SUSPECT (received ACK)", msg->sender_id);
                
                if (proto->callbacks.on_member_alive) {
                    proto->callbacks.on_member_alive(upd->node_id, upd,
                                                    proto->callback_context);
                }
                continue;
            }
            
            // Handle rejoins
            if (existing->status == NODE_STATUS_DEAD && 
                upd->status == NODE_STATUS_ALIVE &&
                upd->incarnation > existing->incarnation) {
                
                cluster_view_release(proto->cluster_view);
                
                cluster_member_t rejoin = {
                    .node_id = upd->node_id,
                    .node_type = upd->node_type,
                    .gossip_port = upd->gossip_port,
                    .data_port = upd->data_port,      // ✅ Update port on rejoin
                    .status = NODE_STATUS_ALIVE,
                    .incarnation = upd->incarnation,
                    .last_seen_ms = time_now_ms()
                };
                safe_strncpy(rejoin.ip_address, upd->ip_address, MAX_IP_LEN);
                
                cluster_view_add(proto->cluster_view, &rejoin);
                
                LOG_INFO("SWIM: Node %u rejoined (inc=%lu, data=%u)", 
                         upd->node_id, upd->incarnation, upd->data_port);
                
                if (proto->callbacks.on_member_alive) {
                    proto->callbacks.on_member_alive(upd->node_id, upd,
                                                    proto->callback_context);
                }
                
            } else if (upd->incarnation > existing->incarnation) {
                // Standard status update
                node_status_t old_status = existing->status;
                cluster_view_release(proto->cluster_view);
                
                cluster_view_update_status(proto->cluster_view, upd->node_id,
                                         upd->status, upd->incarnation);
                
                proto->stats.updates_received++;
                
                // Trigger appropriate callbacks
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
                // Stale update - ignore
                cluster_view_release(proto->cluster_view);
                LOG_DEBUG("SWIM: Ignoring stale ACK update for node %u", upd->node_id);
            }
        }
    }
}

static void handle_join_response(gossip_protocol_t *proto,
                                 const gossip_message_t *msg,
                                 const char *src_ip,
                                 uint16_t src_port) {
    (void)src_ip;
    (void)src_port;
    
    LOG_INFO("SWIM: Processing JOIN_RESPONSE from seed node %u (updates=%u)",
             msg->sender_id, msg->num_updates);
    
    if (msg->num_updates == 0) {
        LOG_WARN("SWIM: JOIN_RESPONSE from %u has no member updates", msg->sender_id);
        return;
    }
    
    // ✅ Process all member updates from seed node
    // This gives us the complete cluster membership at once
    
    int new_members_discovered = 0;
    
    for (uint8_t i = 0; i < msg->num_updates; i++) {
        const gossip_member_update_t *upd = &msg->updates[i];
        
        // Skip ourselves
        if (upd->node_id == proto->my_id) {
            LOG_DEBUG("SWIM: Skipping self in JOIN_RESPONSE");
            continue;
        }
        
        LOG_DEBUG("SWIM: Bootstrap member[%u]: node=%u type=%d ip=%s gossip=%u data=%u",
                  i, upd->node_id, upd->node_type, upd->ip_address,
                  upd->gossip_port, upd->data_port);
        
        cluster_member_t *existing = cluster_view_get(proto->cluster_view, upd->node_id);
        
        if (!existing) {
            // ✅ New member from bootstrap
            cluster_member_t new_member = {
                .node_id = upd->node_id,
                .node_type = upd->node_type,
                .gossip_port = upd->gossip_port,
                .data_port = upd->data_port,      // ✅ Critical for Raft!
                .status = upd->status,
                .incarnation = upd->incarnation,
                .last_seen_ms = time_now_ms()
            };
            safe_strncpy(new_member.ip_address, upd->ip_address, MAX_IP_LEN);
            
            cluster_view_add(proto->cluster_view, &new_member);
            
            new_members_discovered++;
            
            LOG_INFO("SWIM: Bootstrap discovered node %u (type=%d, data=%u)",
                     upd->node_id, upd->node_type, upd->data_port);
            
            proto->stats.updates_received++;
            
            // Notify callback (triggers Raft peer addition)
            if (proto->callbacks.on_member_alive) {
                proto->callbacks.on_member_alive(upd->node_id, upd,
                                                proto->callback_context);
            }
            
        } else {
            // Member already known - update if newer
            if (upd->incarnation > existing->incarnation) {
                node_status_t old_status = existing->status;
                cluster_view_release(proto->cluster_view);
                
                cluster_view_update_status(proto->cluster_view, upd->node_id,
                                         upd->status, upd->incarnation);
                
                LOG_DEBUG("SWIM: Updated existing member %u (inc=%lu)",
                         upd->node_id, upd->incarnation);
                
                proto->stats.updates_received++;
                
                // Trigger callbacks if status changed
                if (upd->status == NODE_STATUS_ALIVE && old_status != NODE_STATUS_ALIVE) {
                    if (proto->callbacks.on_member_alive) {
                        proto->callbacks.on_member_alive(upd->node_id, upd,
                                                        proto->callback_context);
                    }
                }
            } else {
                cluster_view_release(proto->cluster_view);
                LOG_DEBUG("SWIM: Ignoring stale bootstrap update for node %u", upd->node_id);
            }
        }
    }
    
    LOG_INFO("SWIM: Bootstrap complete - discovered %d new members from seed %u",
             new_members_discovered, msg->sender_id);
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
            // JOIN is just a PING from a new node
            handle_ping(proto, msg, src_ip, src_port);
            break;
            
        case GOSSIP_MSG_LEAVE:
            // LEAVE is handled like PING (contains updates)
            handle_ping(proto, msg, src_ip, src_port); 
            break;
        
        case GOSSIP_MSG_JOIN_RESPONSE:
            // ✅ NEW: Handle bootstrap response from seed
            handle_join_response(proto, msg, src_ip, src_port);
            break;
        
        default:
            LOG_DEBUG("SWIM: Unhandled message type %u", msg->msg_type);
            break;
    }
    
    return 0;
}

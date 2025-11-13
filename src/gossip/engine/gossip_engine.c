#define _POSIX_C_SOURCE 200809L

#include "roole/gossip/gossip_engine.h"
#include "roole/transport/udp_transport.h"
#include "roole/core/logger.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

struct gossip_engine {
    node_id_t my_id;
    node_type_t my_type;
    char my_ip[MAX_IP_LEN];
    uint16_t gossip_port;
    uint16_t data_port;
    
    gossip_config_t config;
    
    // Layered architecture
    udp_transport_t *transport;
    gossip_protocol_t *protocol;
    
    cluster_view_t *cluster_view;
    
    member_event_cb event_callback;
    void *event_callback_data;

    pthread_t protocol_thread;
    volatile int shutdown_flag;
};

// ============================================================================
// PROTOCOL CALLBACKS (Bridge between protocol and transport)
// ============================================================================

static void on_member_alive_cb(node_id_t node_id,
                              const gossip_member_update_t *update,
                              void *ctx)
{
    gossip_engine_t *engine = (gossip_engine_t*)ctx;
    
    LOG_INFO("ENGINE: Node %u is ALIVE", node_id);
    
    if (engine->event_callback) {
        engine->event_callback(node_id, update->node_type,
                             update->ip_address, update->data_port,
                             MEMBER_EVENT_JOIN, engine->event_callback_data);
    }
}

static void on_member_suspect_cb(node_id_t node_id,
                                 uint64_t incarnation,
                                 void *ctx)
{
    gossip_engine_t *engine = (gossip_engine_t*)ctx;
    
    LOG_INFO("ENGINE: Node %u is SUSPECT (inc=%lu)", node_id, incarnation);
    
    if (engine->event_callback) {
        cluster_member_t *member = cluster_view_get(engine->cluster_view, node_id);
        if (member) {
            engine->event_callback(node_id, member->node_type,
                                 member->ip_address, member->data_port,
                                 MEMBER_EVENT_FAILED, engine->event_callback_data);
            cluster_view_release(engine->cluster_view);
        }
    }
}

static void on_member_dead_cb(node_id_t node_id, void *ctx)
{
    gossip_engine_t *engine = (gossip_engine_t*)ctx;
    
    LOG_INFO("ENGINE: Node %u is DEAD", node_id);
    
    if (engine->event_callback) {
        cluster_member_t *member = cluster_view_get(engine->cluster_view, node_id);
        if (member) {
            engine->event_callback(node_id, member->node_type,
                                 member->ip_address, member->data_port,
                                 MEMBER_EVENT_LEAVE, engine->event_callback_data);
            cluster_view_release(engine->cluster_view);
        }
    }
}

static void on_send_message_cb(const gossip_message_t *msg,
                              const char *dest_ip,
                              uint16_t dest_port,
                              void *ctx)
{
    gossip_engine_t *engine = (gossip_engine_t*)ctx;
    
    uint8_t buffer[GOSSIP_MAX_PAYLOAD_SIZE];
    ssize_t msg_size = gossip_message_serialize(msg, buffer, sizeof(buffer));
    
    if (msg_size < 0) {
        LOG_ERROR("ENGINE: Failed to serialize message");
        return;
    }
    
    // Broadcast to all peers if dest_ip is NULL
    if (!dest_ip) {
        pthread_rwlock_rdlock(&engine->cluster_view->lock);
        
        for (size_t i = 0; i < engine->cluster_view->count; i++) {
            cluster_member_t *m = &engine->cluster_view->members[i];
            
            if (m->node_id == engine->my_id || m->status == NODE_STATUS_DEAD) {
                continue;
            }
            
            udp_transport_send(engine->transport, buffer, msg_size,
                             m->ip_address, m->gossip_port);
        }
        
        pthread_rwlock_unlock(&engine->cluster_view->lock);
        
        LOG_DEBUG("ENGINE: Broadcast message type %u to all peers", msg->msg_type);
    } else {
        // Send to specific peer
        if (udp_transport_send(engine->transport, buffer, msg_size,
                             dest_ip, dest_port) > 0) {
            LOG_DEBUG("ENGINE: Sent message type %u to %s:%u",
                     msg->msg_type, dest_ip, dest_port);
        } else {
            LOG_WARN("ENGINE: Failed to send message to %s:%u", dest_ip, dest_port);
        }
    }
}

// ============================================================================
// TRANSPORT CALLBACK (Receive messages)
// ============================================================================

static void on_udp_receive(const uint8_t *data, size_t len,
                          const char *src_ip, uint16_t src_port,
                          void *user_data)
{
    gossip_engine_t *engine = (gossip_engine_t*)user_data;
    
    LOG_DEBUG("ENGINE: Received %zu bytes from %s:%u", len, src_ip, src_port);
    
    if (len < 16) {
        LOG_WARN("ENGINE: Malformed gossip packet (too small: %zu bytes)", len);
        return;
    }
    
    gossip_message_t msg;
    if (gossip_message_deserialize(data, len, &msg) != 0) {
        LOG_ERROR("ENGINE: Failed to deserialize gossip message");
        return;
    }
    
    LOG_DEBUG("ENGINE: Deserialized message: type=%u sender=%u updates=%u",
              msg.msg_type, msg.sender_id, msg.num_updates);
    
    // Pass to protocol layer for processing
    gossip_protocol_handle_message(engine->protocol, &msg, src_ip, src_port);
}

// ============================================================================
// PROTOCOL THREAD
// ============================================================================

static void* protocol_loop_thread(void *arg)
{
    gossip_engine_t *engine = (gossip_engine_t*)arg;
    
    logger_push_component("gossip:protocol");
    LOG_INFO("Protocol loop thread started (period=%ums)", 
             engine->config.protocol_period_ms);
    
    uint64_t round = 0;
    
    while (!engine->shutdown_flag) {
        round++;
        
        LOG_DEBUG("=== SWIM Protocol Round %lu ===", round);
        
        // Run one SWIM round (ping random peer)
        gossip_protocol_run_swim_round(engine->protocol);
        
        // Check for timeouts (ACK and SUSPECT)
        gossip_protocol_check_timeouts(engine->protocol);
        
        // Periodic statistics
        if (round % 10 == 0) {
            pthread_rwlock_rdlock(&engine->cluster_view->lock);
            
            size_t alive = 0, suspect = 0, dead = 0;
            for (size_t i = 0; i < engine->cluster_view->count; i++) {
                switch (engine->cluster_view->members[i].status) {
                    case NODE_STATUS_ALIVE: alive++; break;
                    case NODE_STATUS_SUSPECT: suspect++; break;
                    case NODE_STATUS_DEAD: dead++; break;
                }
            }
            
            LOG_INFO("Cluster: %zu members (%zu alive, %zu suspect, %zu dead)",
                     engine->cluster_view->count, alive, suspect, dead);
            
            pthread_rwlock_unlock(&engine->cluster_view->lock);
            
            gossip_protocol_stats_t stats;
            gossip_protocol_get_stats(engine->protocol, &stats);
            LOG_INFO("Protocol: pings=%lu acks=%lu timeouts=%lu suspect=%lu dead=%lu",
                     stats.pings_sent, stats.acks_received, stats.ack_timeouts,
                     stats.suspect_count, stats.dead_count);
        }
        
        usleep(engine->config.protocol_period_ms * 1000);
    }
    
    LOG_INFO("Protocol loop thread stopped");
    logger_pop_component();
    return NULL;
}

// ============================================================================
// PUBLIC API
// ============================================================================

gossip_engine_t* gossip_engine_create(
    node_id_t my_id,
    node_type_t my_type,
    const char *bind_addr,
    uint16_t gossip_port,
    uint16_t data_port,
    const gossip_config_t *config,
    cluster_view_t *cluster_view,
    member_event_cb event_callback,
    void *user_data)
{
    if (!bind_addr || !cluster_view) {
        LOG_ERROR("Invalid parameters for gossip engine");
        return NULL;
    }
    
    gossip_engine_t *engine = calloc(1, sizeof(gossip_engine_t));
    if (!engine) {
        LOG_ERROR("Failed to allocate gossip engine");
        return NULL;
    }
    
    engine->my_id = my_id;
    engine->my_type = my_type;
    safe_strncpy(engine->my_ip, bind_addr, MAX_IP_LEN);
    engine->gossip_port = gossip_port;
    engine->data_port = data_port;
    engine->cluster_view = cluster_view;
    engine->event_callback = event_callback;
    engine->event_callback_data = user_data;
    engine->shutdown_flag = 0;
    
    if (config) {
        engine->config = *config;
    } else {
        engine->config = gossip_default_config();
    }
    
    // Create UDP transport
    engine->transport = udp_transport_create(bind_addr, gossip_port);
    if (!engine->transport) {
        LOG_ERROR("Failed to create UDP transport");
        gossip_engine_shutdown(engine);
        return NULL;
    }
    
    // Create protocol layer
    gossip_protocol_callbacks_t protocol_callbacks = {
        .on_member_alive = on_member_alive_cb,
        .on_member_suspect = on_member_suspect_cb,
        .on_member_dead = on_member_dead_cb,
        .on_send_message = on_send_message_cb
    };
    
    engine->protocol = gossip_protocol_create(
        my_id, my_type, bind_addr, gossip_port, data_port,
        &engine->config, cluster_view,
        &protocol_callbacks, engine
    );
    
    if (!engine->protocol) {
        LOG_ERROR("Failed to create gossip protocol");
        udp_transport_destroy(engine->transport);
        gossip_engine_shutdown(engine);
        return NULL;
    }

    LOG_INFO("Gossip engine created (node_id=%u, type=%d, port=%u)",
             my_id, my_type, gossip_port);
    
    return engine;
}

int gossip_engine_start(gossip_engine_t *engine)
{
    if (!engine) return -1;
    
    // Start UDP receiver
    if (udp_transport_start_receiver(engine->transport, on_udp_receive, engine) != 0) {
        LOG_ERROR("Failed to start UDP receiver");
        return -1;
    }
    
    // Start protocol loop thread
    if (pthread_create(&engine->protocol_thread, NULL, protocol_loop_thread, engine) != 0) {
        LOG_ERROR("Failed to start protocol thread");
        udp_transport_stop_receiver(engine->transport);
        return -1;
    }
    
    LOG_INFO("Gossip engine started");
    return 0;
}

int gossip_engine_add_seed(gossip_engine_t *engine,
                          const char *seed_ip,
                          uint16_t seed_port)
{
    if (!engine || !seed_ip) return -1;
    
    LOG_INFO("Adding seed node: %s:%u", seed_ip, seed_port);
    
    gossip_protocol_add_seed(engine->protocol, seed_ip, seed_port);
    
    return 0;
}

int gossip_engine_announce_join(gossip_engine_t *engine)
{
    if (!engine) return -1;
    
    LOG_INFO("Announcing join to cluster");
    
    gossip_protocol_announce_join(engine->protocol);
    
    return 0;
}

int gossip_engine_leave(gossip_engine_t *engine)
{
    if (!engine) return -1;
    
    LOG_INFO("Gracefully leaving cluster");
    
    gossip_protocol_announce_leave(engine->protocol);
    
    sleep(1); // Give time for LEAVE to propagate
    
    return 0;
}

void gossip_engine_shutdown(gossip_engine_t *engine)
{
    if (!engine) return;
    
    LOG_INFO("Shutting down gossip engine");
    
    engine->shutdown_flag = 1;
    
    // Stop threads
    pthread_join(engine->protocol_thread, NULL);
    
    // Cleanup layers
    if (engine->protocol) {
        gossip_protocol_destroy(engine->protocol);
    }
    
    if (engine->transport) {
        udp_transport_destroy(engine->transport);
    }

    free(engine);
    
    LOG_INFO("Gossip engine shutdown complete");
}

void gossip_engine_set_callback(gossip_engine_t *engine,
                                member_event_cb callback,
                                void *user_data)
{
    if (!engine) return;
    
    engine->event_callback = callback;
    engine->event_callback_data = user_data;
    
    LOG_DEBUG("Gossip engine callback updated");
}
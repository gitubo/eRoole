// src/cluster/membership.c
// PHASE 1: Refactored to use shared cluster_view instead of internal_view

#define _POSIX_C_SOURCE 200809L

#include "roole/cluster/membership.h"
#include "roole/core/logger.h"
#include "roole/gossip/gossip_engine.h" 
#include <stdlib.h>
#include <unistd.h>

struct membership_handle {
    node_id_t my_id;
    node_type_t my_type;
    char bind_addr[MAX_IP_LEN];
    uint16_t gossip_port;
    uint16_t data_port;
    
    cluster_view_t *shared_view;    
    member_event_cb event_callback;
    void *event_callback_user_data;
    
    gossip_engine_t *gossip_engine;
    int shutdown_flag;
};

// ============================================================================
// MEMBERSHIP HANDLE IMPLEMENTATION
// ============================================================================

int membership_init(membership_handle_t **handle, 
                   node_id_t my_id, 
                   node_type_t my_type, 
                   const char *bind_addr, 
                   uint16_t gossip_port, 
                   uint16_t data_port,
                   cluster_view_t *shared_view) {  // ✅ NEW parameter
    
    // ✅ VALIDATION: Ensure shared_view is provided
    if (!handle || !shared_view) {
        LOG_ERROR("membership_init: handle or shared_view is NULL");
        return RESULT_ERR_INVALID;
    }
    
    membership_handle_t *h = safe_calloc(1, sizeof(membership_handle_t));
    if (!h) return RESULT_ERR_NOMEM;
    
    h->my_id = my_id;
    h->my_type = my_type;
    safe_strncpy(h->bind_addr, bind_addr ? bind_addr : "0.0.0.0", MAX_IP_LEN);
    h->gossip_port = gossip_port;
    h->data_port = data_port;
    h->shutdown_flag = 0;
    
    // ✅ PHASE 1 CHANGE: Store pointer to shared view instead of creating internal copy
    // OLD: cluster_view_init(&h->internal_view, MAX_CLUSTER_NODES);
    h->shared_view = shared_view;
    
    LOG_DEBUG("membership_init: Using shared cluster_view at %p", (void*)shared_view);
    
    // Add self to shared cluster view (not internal copy)
    cluster_member_t self = {
        .node_id = my_id,
        .node_type = my_type,
        .gossip_port = gossip_port,
        .data_port = data_port,
        .status = NODE_STATUS_ALIVE,
        .incarnation = 0,
        .last_seen_ms = time_now_ms()
    };
    safe_strncpy(self.ip_address, h->bind_addr, MAX_IP_LEN);
    
    // ✅ CHANGED: Add to shared_view instead of internal_view
    cluster_view_add(h->shared_view, &self);
    
    gossip_config_t gossip_config = gossip_default_config();
    
    // ✅ CHANGED: Pass shared_view to gossip engine (already correct in original code)
    h->gossip_engine = gossip_engine_create(
        my_id,
        my_type,
        h->bind_addr,
        gossip_port,
        data_port,
        &gossip_config,
        h->shared_view,  // ✅ Use shared_view
        NULL,
        NULL
    );
    
    if (!h->gossip_engine) {
        LOG_ERROR("Failed to initialize gossip engine");
        // ❌ REMOVED: cluster_view_destroy(&h->internal_view);
        safe_free(h);
        return RESULT_ERR_INVALID;
    }
    
    if (gossip_engine_start(h->gossip_engine) != 0) {
        LOG_ERROR("Failed to start gossip engine");
        gossip_engine_shutdown(h->gossip_engine);
        // ❌ REMOVED: cluster_view_destroy(&h->internal_view);
        safe_free(h);
        return RESULT_ERR_INVALID;
    }
    
    *handle = h;
    LOG_INFO("Membership initialized (node_id=%u, type=%d, gossip_port=%u, shared_view=%p)", 
             my_id, my_type, gossip_port, (void*)shared_view);
    return RESULT_OK;
}

int membership_join(membership_handle_t *handle, const char *seed_addr, uint16_t seed_port) {
    if (!handle || !seed_addr) return RESULT_ERR_INVALID;
    
    LOG_INFO("Joining cluster via seed %s:%u", seed_addr, seed_port);
    
    if (gossip_engine_add_seed(handle->gossip_engine, seed_addr, seed_port) != 0) {
        LOG_ERROR("Failed to send JOIN to seed");
        return RESULT_ERR_NETWORK;
    }
    
    gossip_engine_announce_join(handle->gossip_engine);
    
    LOG_INFO("JOIN sent to seed, waiting for cluster view to populate...");
    return RESULT_OK;
}

int membership_set_callback(membership_handle_t *handle, member_event_cb callback, void *user_data) {
    if (!handle) return RESULT_ERR_INVALID;
    
    handle->event_callback = callback;
    handle->event_callback_user_data = user_data;
    
    if (handle->gossip_engine) {
        gossip_engine_set_callback(handle->gossip_engine, callback, user_data);
    }
    
    return RESULT_OK;
}

int membership_leave(membership_handle_t *handle) {
    if (!handle) return RESULT_ERR_INVALID;
    
    LOG_INFO("Gracefully leaving cluster");
    
    if (handle->gossip_engine) {
        gossip_engine_leave(handle->gossip_engine);
    }
    
    sleep(1);
    
    return RESULT_OK;
}

void membership_shutdown(membership_handle_t *handle) {
    if (!handle) return;
    
    LOG_INFO("Shutting down membership...");
    
    handle->shutdown_flag = 1;
    
    if (handle->gossip_engine) {
        gossip_engine_shutdown(handle->gossip_engine);
        handle->gossip_engine = NULL;
    }
    
    // ❌ REMOVED: cluster_view_destroy(&handle->internal_view);
    // ✅ RATIONALE: shared_view is owned by caller (node_state), not membership
    LOG_DEBUG("Membership shutdown: shared_view ownership retained by caller");
    
    safe_free(handle);
    
    LOG_INFO("Membership shutdown complete");
}

size_t membership_get_members(membership_handle_t *handle, 
                              cluster_member_t *out_members, 
                              size_t max_count) {
    if (!handle || !out_members || max_count == 0) return 0;
    
    // ✅ CHANGED: Access shared_view
    pthread_rwlock_rdlock(&handle->shared_view->lock);
    
    size_t count = ROOLE_MIN(handle->shared_view->count, max_count);
    memcpy(out_members, handle->shared_view->members, 
           count * sizeof(cluster_member_t));
    
    pthread_rwlock_unlock(&handle->shared_view->lock);
    
    return count;
}

#include "roole/cluster/cluster_view.h"
#include "roole/logger/logger.h"
#include "roole/core/common.h"
#include <stdlib.h>

// ============================================================================
// CLUSTER VIEW IMPLEMENTATION (Unchanged)
// ============================================================================

int cluster_view_init(cluster_view_t *view, size_t capacity) {
    if (!view || capacity == 0) return RESULT_ERR_INVALID;
    
    memset(view, 0, sizeof(cluster_view_t));
    
    view->members = safe_calloc(capacity, sizeof(cluster_member_t));
    if (!view->members) {
        return RESULT_ERR_NOMEM;
    }
    
    view->capacity = capacity;
    view->count = 0;
    
    if (pthread_rwlock_init(&view->lock, NULL) != 0) {
        safe_free(view->members);
        return RESULT_ERR_INVALID;
    }
    
    LOG_INFO("Cluster view initialized (capacity: %zu)", capacity);
    return RESULT_OK;
}

void cluster_view_destroy(cluster_view_t *view) {
    if (!view) return;
    
    pthread_rwlock_wrlock(&view->lock);
    
    safe_free(view->members);
    view->members = NULL;
    view->count = 0;
    view->capacity = 0;
    
    pthread_rwlock_unlock(&view->lock);
    pthread_rwlock_destroy(&view->lock);
    
    LOG_INFO("Cluster view destroyed");
}

int cluster_view_add(cluster_view_t *view, const cluster_member_t *member) {
    if (!view || !member) return RESULT_ERR_INVALID;
    
    pthread_rwlock_wrlock(&view->lock);
    
    // Check if node already exists
    for (size_t i = 0; i < view->count; i++) {
        if (view->members[i].node_id == member->node_id) {
            // Node exists - update it if new incarnation or status change
            if (member->status == NODE_STATUS_ALIVE && 
                view->members[i].status == NODE_STATUS_DEAD) {
                // Rejoining node - update everything
                view->members[i] = *member;
                view->members[i].last_seen_ms = time_now_ms();
                //view->members[i].incarnation = member->incarnation + 1;
                pthread_rwlock_unlock(&view->lock);
                LOG_INFO("Node %u rejoined cluster (was DEAD, now ALIVE, incarnation=%lu)", 
                         member->node_id, view->members[i].incarnation);
                return RESULT_OK;
            }
            else if (member->incarnation >= view->members[i].incarnation) {
                view->members[i] = *member;
                view->members[i].last_seen_ms = time_now_ms();
                pthread_rwlock_unlock(&view->lock);
                LOG_DEBUG("Updated existing member %u (incarnation=%lu)", 
                          member->node_id, member->incarnation);
                return RESULT_OK;
            }
            pthread_rwlock_unlock(&view->lock);
            LOG_DEBUG("Ignoring stale update for node %u (inc %lu <= %lu)", 
                      member->node_id, member->incarnation, view->members[i].incarnation);
            return RESULT_OK;
        }
    }
    
    // New node - add it
    if (view->count >= view->capacity) {
        pthread_rwlock_unlock(&view->lock);
        LOG_ERROR("Cluster view full (capacity: %zu)", view->capacity);
        return RESULT_ERR_FULL;
    }
    
    view->members[view->count] = *member;
    if (member->status == NODE_STATUS_ALIVE) {
        view->members[view->count].last_seen_ms = time_now_ms();
    } else {
        // Keep the timestamp from the member parameter (for testing or rejoins)
        view->members[view->count].last_seen_ms = member->last_seen_ms;
    }
    view->count++;
    
    pthread_rwlock_unlock(&view->lock);
    
    LOG_INFO("Added member %u (%s:%u, type=%d)", 
             member->node_id, member->ip_address, member->gossip_port, member->node_type);
    return RESULT_OK;
}

int cluster_view_update_status(cluster_view_t *view, node_id_t node_id, 
                               node_status_t status, uint64_t incarnation) {
    if (!view) return RESULT_ERR_INVALID;
    
    pthread_rwlock_wrlock(&view->lock);
    
    for (size_t i = 0; i < view->count; i++) {
        if (view->members[i].node_id == node_id) {
            if (incarnation >= view->members[i].incarnation) {
                node_status_t old_status = view->members[i].status;
                
                view->members[i].status = status;
                view->members[i].incarnation = incarnation;
                
                if (status == NODE_STATUS_ALIVE) {
                    view->members[i].last_seen_ms = time_now_ms();
                } else if (status == NODE_STATUS_SUSPECT && old_status == NODE_STATUS_ALIVE) {
                    view->members[i].last_seen_ms = time_now_ms();
                }
                
                pthread_rwlock_unlock(&view->lock);
                LOG_DEBUG("Updated node %u status to %d (incarnation %lu)", 
                          node_id, status, incarnation);
                return RESULT_OK;
            }
            pthread_rwlock_unlock(&view->lock);
            return RESULT_OK;
        }
    }
    
    pthread_rwlock_unlock(&view->lock);
    LOG_WARN("Node %u not found for status update", node_id);
    return RESULT_ERR_NOTFOUND;
}

int cluster_view_remove(cluster_view_t *view, node_id_t node_id) {
    if (!view) return RESULT_ERR_INVALID;
    
    pthread_rwlock_wrlock(&view->lock);
    
    for (size_t i = 0; i < view->count; i++) {
        if (view->members[i].node_id == node_id) {
            if (i < view->count - 1) {
                memmove(&view->members[i], &view->members[i + 1], 
                       (view->count - i - 1) * sizeof(cluster_member_t));
            }
            view->count--;
            pthread_rwlock_unlock(&view->lock);
            LOG_INFO("Removed member %u", node_id);
            return RESULT_OK;
        }
    }
    
    pthread_rwlock_unlock(&view->lock);
    return RESULT_ERR_NOTFOUND;
}

cluster_member_t* cluster_view_get(cluster_view_t *view, node_id_t node_id) {
    if (!view) return NULL;
    
    pthread_rwlock_rdlock(&view->lock);
    
    for (size_t i = 0; i < view->count; i++) {
        if (view->members[i].node_id == node_id) {
            return &view->members[i];
        }
    }
    
    pthread_rwlock_unlock(&view->lock);
    return NULL;
}

void cluster_view_release(cluster_view_t *view) {
    if (view) {
        pthread_rwlock_unlock(&view->lock);
    }
}

size_t cluster_view_list_by_type(cluster_view_t *view, node_type_t type,
                                 node_id_t *out_node_ids, size_t max_count) {
    if (!view || !out_node_ids || max_count == 0) return 0;
    
    pthread_rwlock_rdlock(&view->lock);
    
    size_t found = 0;
    for (size_t i = 0; i < view->count && found < max_count; i++) {
        if (view->members[i].node_type == type) {
            out_node_ids[found++] = view->members[i].node_id;
        }
    }
    
    pthread_rwlock_unlock(&view->lock);
    
    return found;
}

size_t cluster_view_list_alive(cluster_view_t *view, node_type_t type,
                               node_id_t *out_node_ids, size_t max_count) {
    if (!view || !out_node_ids || max_count == 0) return 0;
    
    pthread_rwlock_rdlock(&view->lock);
    
    size_t found = 0;
    for (size_t i = 0; i < view->count && found < max_count; i++) {
        if (view->members[i].node_type == type && 
            view->members[i].status == NODE_STATUS_ALIVE) {
            out_node_ids[found++] = view->members[i].node_id;
        }
    }
    
    pthread_rwlock_unlock(&view->lock);
    
    return found;
}

void cluster_view_dump(cluster_view_t *view, const char *label) {
    if (!view) return;
    
    pthread_rwlock_rdlock(&view->lock);
    
    LOG_INFO("========================================");
    LOG_INFO("Cluster View Dump: %s", label ? label : "");
    LOG_INFO("========================================");
    LOG_INFO("Total members: %zu (capacity: %zu)", view->count, view->capacity);
    
    if (view->count == 0) {
        LOG_INFO("  (empty)");
    } else {
        for (size_t i = 0; i < view->count; i++) {
            cluster_member_t *m = &view->members[i];
            const char *status_str = (m->status == NODE_STATUS_ALIVE) ? "ALIVE" :
                                    (m->status == NODE_STATUS_SUSPECT) ? "SUSPECT" : "DEAD";
            const char *type_str = (m->node_type == NODE_TYPE_ROUTER) ? "ROUTER" : "WORKER";
            
            LOG_INFO("  [%zu] node_id=%u type=%s status=%s ip=%s:%u data=%u inc=%lu last_seen=%lu",
                    i, m->node_id, type_str, status_str,
                    m->ip_address, m->gossip_port, m->data_port,
                    m->incarnation, m->last_seen_ms);
        }
    }
    
    LOG_INFO("========================================");
    
    pthread_rwlock_unlock(&view->lock);
}

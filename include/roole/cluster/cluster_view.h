#ifndef ROOLE_CLUSTER_VIEW_H
#define ROOLE_CLUSTER_VIEW_H

#include "roole/cluster/cluster_types.h" 
#include <pthread.h>

// Cluster view (shared state)
typedef struct cluster_view {
    cluster_member_t *members;
    size_t count;
    size_t capacity;
    pthread_rwlock_t lock;
} cluster_view_t;

/**
 * Initialize cluster view
 * @param view View structure
 * @param capacity Maximum number of members
 * @return 0 on success, error code on failure
 */
int cluster_view_init(cluster_view_t *view, size_t capacity);

/**
 * Destroy cluster view
 * @param view View structure
 */
void cluster_view_destroy(cluster_view_t *view);

/**
 * Add or update member
 * If member exists, updates it. If not, adds new member.
 * Thread-safe: acquires write lock
 * @param view View structure
 * @param member Member to add/update
 * @return 0 on success, error code on failure
 */
int cluster_view_add(cluster_view_t *view, const cluster_member_t *member);

/**
 * Update member status
 * Thread-safe: acquires write lock
 * @param view View structure
 * @param node_id Node ID to update
 * @param status New status
 * @param incarnation New incarnation
 * @return 0 on success, error code on failure
 */
int cluster_view_update_status(cluster_view_t *view, node_id_t node_id,
                                node_status_t status, uint64_t incarnation);

/**
 * Remove member
 * Thread-safe: acquires write lock
 * @param view View structure
 * @param node_id Node ID to remove
 * @return 0 on success, error code on failure
 */
int cluster_view_remove(cluster_view_t *view, node_id_t node_id);

/**
 * Get member by ID
 * Returns pointer to member (read lock held)
 * Caller MUST call cluster_view_release() when done
 * Thread-safe: acquires read lock
 * @param view View structure
 * @param node_id Node ID to find
 * @return Pointer to member, or NULL if not found
 */
cluster_member_t* cluster_view_get(cluster_view_t *view, node_id_t node_id);

/**
 * Release read lock
 * Must be called after cluster_view_get()
 * @param view View structure
 */
void cluster_view_release(cluster_view_t *view);

/**
 * List all members of given type
 * Thread-safe: acquires read lock
 * @param view View structure
 * @param type Node type to filter by
 * @param out_node_ids Output array of node IDs
 * @param max_count Maximum number of IDs to return
 * @return Number of IDs returned
 */
size_t cluster_view_list_by_type(cluster_view_t *view, node_type_t type,
                                  node_id_t *out_node_ids, size_t max_count);

/**
 * List alive members of given type
 * Thread-safe: acquires read lock
 * @param view View structure
 * @param type Node type to filter by
 * @param out_node_ids Output array of node IDs
 * @param max_count Maximum number of IDs to return
 * @return Number of IDs returned
 */
size_t cluster_view_list_alive(cluster_view_t *view, node_type_t type,
                                node_id_t *out_node_ids, size_t max_count);

/**
 * Debug helper: dump cluster view to log
 * @param view View structure
 * @param label Label for dump
 */
void cluster_view_dump(cluster_view_t *view, const char *label);

#endif // ROOLE_CLUSTER_VIEW_H
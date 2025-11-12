#ifndef ROOLE_CLUSTER_MEMBERSHIP_H
#define ROOLE_CLUSTER_MEMBERSHIP_H

#include "roole/cluster/cluster_types.h"
#include "roole/cluster/cluster_view.h"

// Forward declare gossip_engine (break the include cycle)
typedef struct gossip_engine gossip_engine_t;

// Opaque handle
typedef struct membership_handle membership_handle_t;

/**
 * Initialize membership
 */
int membership_init(
    membership_handle_t **handle,
    node_id_t my_id,
    node_type_t my_type,
    const char *bind_addr,
    uint16_t gossip_port,
    uint16_t data_port,
    cluster_view_t *shared_view
);

/**
 * Initialize membership
 * Creates gossip engine and starts membership tracking
 * @param handle Output handle pointer
 * @param my_id This node's ID
 * @param my_type This node's type
 * @param bind_addr Address to bind
 * @param gossip_port Gossip port
 * @param data_port Data port (metadata only)
 * @param shared_view Shared cluster view (owned by caller)
 * @return 0 on success, error code on failure
 */
int membership_init(
    membership_handle_t **handle,
    node_id_t my_id,
    node_type_t my_type,
    const char *bind_addr,
    uint16_t gossip_port,
    uint16_t data_port,
    cluster_view_t *shared_view
);

/**
 * Join cluster via seed node
 * @param handle Membership handle
 * @param seed_addr Seed address (ip:port)
 * @param seed_port Seed port
 * @return 0 on success, error code on failure
 */
int membership_join(membership_handle_t *handle, const char *seed_addr, uint16_t seed_port);

/**
 * Set event callback
 * @param handle Membership handle
 * @param callback Callback function
 * @param user_data User data for callback
 * @return 0 on success
 */
int membership_set_callback(membership_handle_t *handle, member_event_cb callback, void *user_data);

/**
 * Gracefully leave cluster
 * @param handle Membership handle
 * @return 0 on success
 */
int membership_leave(membership_handle_t *handle);

/**
 * Shutdown membership
 * Stops gossip engine, frees resources
 * @param handle Membership handle
 */
void membership_shutdown(membership_handle_t *handle);

/**
 * Get current cluster members
 * @param handle Membership handle
 * @param out_members Output array
 * @param max_count Maximum count
 * @return Number of members returned
 */
size_t membership_get_members(membership_handle_t *handle,
                               cluster_member_t *out_members,
                               size_t max_count);

#endif // ROOLE_CLUSTER_MEMBERSHIP_H
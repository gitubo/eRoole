// include/roole/gossip/gossip_engine.h
// High-level gossip engine (coordinates transport + protocol)

#ifndef ROOLE_GOSSIP_ENGINE_H
#define ROOLE_GOSSIP_ENGINE_H

#include "roole/gossip/gossip_protocol.h"
#include "roole/transport/udp_transport.h"
#include "roole/cluster/cluster_types.h"   
#include "roole/cluster/cluster_view.h"

typedef struct gossip_engine gossip_engine_t;

/**
 * Create gossip engine
 * Initializes transport and protocol layers
 * @param my_id This node's ID
 * @param my_type This node's type
 * @param bind_addr Address to bind UDP socket
 * @param gossip_port UDP port for gossip
 * @param data_port TCP port for RPC (metadata only)
 * @param config Protocol configuration (NULL for defaults)
 * @param cluster_view Shared cluster view
 * @param event_callback Membership event callback (optional)
 * @param user_data User data for callback
 * @return Engine handle, or NULL on error
 */
gossip_engine_t* gossip_engine_create(
    node_id_t my_id,
    node_type_t my_type,
    const char *bind_addr,
    uint16_t gossip_port,
    uint16_t data_port,
    const gossip_config_t *config,
    cluster_view_t *cluster_view,
    member_event_cb event_callback,
    void *user_data
);

/**
 * Start gossip engine
 * Starts UDP receiver thread and protocol loop thread
 * @param engine Engine handle
 * @return 0 on success, -1 on error
 */
int gossip_engine_start(gossip_engine_t *engine);

/**
 * Add seed node for bootstrap
 * @param engine Engine handle
 * @param seed_ip Seed node IP
 * @param seed_port Seed node gossip port
 * @return 0 on success, -1 on error
 */
int gossip_engine_add_seed(gossip_engine_t *engine, const char *seed_ip, uint16_t seed_port);

/**
 * Announce join to cluster
 * @param engine Engine handle
 * @return 0 on success, -1 on error
 */
int gossip_engine_announce_join(gossip_engine_t *engine);

/**
 * Gracefully leave cluster
 * @param engine Engine handle
 * @return 0 on success, -1 on error
 */
int gossip_engine_leave(gossip_engine_t *engine);

/**
 * Shutdown gossip engine
 * Stops threads, closes sockets, frees memory
 * @param engine Engine handle
 */
void gossip_engine_shutdown(gossip_engine_t *engine);

/**
 * Set event callback (can be changed after creation)
 * @param engine Engine handle
 * @param callback New callback function
 * @param user_data New user data
 */
void gossip_engine_set_callback(gossip_engine_t *engine, 
                                member_event_cb callback,
                                void *user_data);

#endif // ROOLE_GOSSIP_ENGINE_H
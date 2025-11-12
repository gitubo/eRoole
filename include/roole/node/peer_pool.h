// include/roole/node/peer_pool.h
// Peer connection pool (unchanged from original)

#ifndef ROOLE_NODE_PEER_POOL_H
#define ROOLE_NODE_PEER_POOL_H

#include "roole/core/common.h"
#include "roole/cluster/cluster_view.h"
#include "roole/rpc/rpc_channel.h"
#include "roole/node/node_capabilities.h"
#include <pthread.h>

#define MAX_PEERS 512

// Peer information
typedef struct {
    node_id_t node_id;
    node_type_t node_type;
    char ip[MAX_IP_LEN];
    uint16_t gossip_port;
    uint16_t data_port;
    node_status_t status;
    
    uint32_t active_executions;
    float load_score;
    uint64_t last_seen_ms;
    
    rpc_channel_t *data_channel;  // RPC channel for peer communication
    node_capabilities_t capabilities;
} peer_info_t;

// Peer pool
typedef struct {
    peer_info_t *peers;
    size_t count;
    size_t capacity;
    pthread_mutex_t lock;
} peer_pool_t;

/**
 * Initialize peer pool
 * @param pool Pool structure
 * @param capacity Maximum peers
 * @return 0 on success, error code on failure
 */
int peer_pool_init(peer_pool_t *pool, size_t capacity);

/**
 * Destroy peer pool
 * @param pool Pool structure
 */
void peer_pool_destroy(peer_pool_t *pool);

/**
 * Add peer to pool
 * @param pool Pool structure
 * @param node_id Node ID
 * @param ip IP address
 * @param gossip_port Gossip port
 * @param data_port Data port
 * @return 0 on success, error code on failure
 */
int peer_pool_add(peer_pool_t *pool, node_id_t node_id, const char *ip,
                  uint16_t gossip_port, uint16_t data_port);

/**
 * Remove peer from pool
 * @param pool Pool structure
 * @param node_id Node ID to remove
 * @return 0 on success, error code on failure
 */
int peer_pool_remove(peer_pool_t *pool, node_id_t node_id);

/**
 * Update peer status
 * @param pool Pool structure
 * @param node_id Node ID
 * @param status New status
 * @return 0 on success, error code on failure
 */
int peer_pool_update_status(peer_pool_t *pool, node_id_t node_id, node_status_t status);

/**
 * Get peer by ID
 * Returns pointer (lock held) - caller must call peer_pool_release()
 * @param pool Pool structure
 * @param node_id Node ID
 * @return Peer pointer, or NULL if not found
 */
peer_info_t* peer_pool_get(peer_pool_t *pool, node_id_t node_id);

/**
 * Release lock after peer_pool_get()
 * @param pool Pool structure
 */
void peer_pool_release(peer_pool_t *pool);

/**
 * Select least loaded peer
 * @param pool Pool structure
 * @return Node ID of least loaded peer, or 0 if none available
 */
node_id_t peer_pool_select_least_loaded(peer_pool_t *pool);

/**
 * Select peer using round-robin
 * @param pool Pool structure
 * @return Node ID, or 0 if none available
 */
node_id_t peer_pool_select_round_robin(peer_pool_t *pool);

/**
 * List alive peers
 * @param pool Pool structure
 * @param out_peer_ids Output array
 * @param max_count Maximum count
 * @return Number of peers returned
 */
size_t peer_pool_list_alive(peer_pool_t *pool, node_id_t *out_peer_ids, size_t max_count);

/**
 * List peers by capability
 * @param pool Pool structure
 * @param can_execute Filter by execution capability
 * @param out_peer_ids Output array
 * @param max_count Maximum count
 * @return Number of peers returned
 */
size_t peer_pool_list_by_capability(peer_pool_t *pool, int can_execute,
                                    node_id_t *out_peer_ids, size_t max_count);

/**
 * Update peer load
 * @param pool Pool structure
 * @param node_id Node ID
 * @param active_execs Number of active executions
 * @param load_score Load score
 * @return 0 on success
 */
int peer_pool_update_load(peer_pool_t *pool, node_id_t node_id,
                          uint32_t active_execs, float load_score);

/**
 * Update peer capabilities
 * @param pool Pool structure
 * @param node_id Node ID
 * @param caps Capabilities
 * @return 0 on success
 */
int peer_pool_update_capabilities(peer_pool_t *pool, node_id_t node_id,
                                  const node_capabilities_t *caps);

#endif // ROOLE_NODE_PEER_POOL_H
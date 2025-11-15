// include/roole/node/node_state.h

#ifndef ROOLE_NODE_STATE_H
#define ROOLE_NODE_STATE_H

#include "roole/core/common.h"
#include "roole/config/config.h"
#include "roole/datastore/datastore.h"
#include "roole/metrics/metrics.h"
#include "roole/metrics/metrics_server.h"
#include "roole/core/event_bus.h"
#include "roole/cluster/cluster_view.h"
#include "roole/cluster/membership.h"
#include "roole/node/peer_pool.h"
#include "roole/node/node_capabilities.h"
#include <pthread.h>

// Node identity (immutable after initialization)
typedef struct node_identity {
    node_id_t node_id;
    node_type_t node_type;
    char cluster_name[64];
    char bind_addr[MAX_IP_LEN];
    uint16_t gossip_port;
    uint16_t data_port;
    uint16_t ingress_port;
    uint16_t metrics_port;
} node_identity_t;

// Simplified node state (no execution engine)
typedef struct node_state {
    // Identity (immutable)
    node_identity_t identity;
    node_capabilities_t capabilities;
    
    // Core subsystem: DATASTORE ONLY
    datastore_t *datastore;
    
    // Cluster membership (owns the view)
    cluster_view_t *cluster_view;
    membership_handle_t *membership;
    
    // Peer tracking
    peer_pool_t *peer_pool;
    
    // Observability
    metrics_registry_t *metrics_registry;
    metrics_server_t *metrics_server;
    event_bus_t *event_bus;
    
    // Metrics references (for fast access)
    metrics_t *metric_cluster_members_total;
    metrics_t *metric_cluster_members_active;
    metrics_t *metric_cluster_members_suspect;
    metrics_t *metric_cluster_members_dead;
    metrics_t *metric_datastore_size;
    metrics_t *metric_datastore_bytes;
    metrics_t *metric_datastore_sets;
    metrics_t *metric_datastore_gets;
    metrics_t *metric_datastore_unsets;
    metrics_t *metric_uptime_seconds;
    
    histogram_metric_t *histogram_gossip_rtt;
    histogram_metric_t *histogram_datastore_op_duration;
    
    // Lifecycle
    uint64_t start_time_ms;
    volatile int shutdown_flag;
    
    // Background threads
    pthread_t cleanup_thread;
    pthread_t metrics_update_thread;
    
    // Statistics (atomic counters)
    _Atomic uint64_t datastore_ops_total;
    
    // Synchronization for startup
    volatile int rpc_server_ready;
    pthread_mutex_t rpc_ready_lock;
    pthread_cond_t rpc_ready_cond;
} node_state_t;

/**
 * Initialize node state
 * Allocates and initializes all subsystems (no executor threads)
 * @param state Output state pointer
 * @param config Configuration
 * @return result_t (RESULT_OK or error)
 */
result_t node_state_init(node_state_t **state, const roole_config_t *config);

/**
 * Start node
 * Starts background threads, RPC servers (no executors)
 * @param state Node state
 * @return result_t
 */
result_t node_state_start(node_state_t *state);

/**
 * Bootstrap node (join cluster)
 * @param state Node state
 * @param config Configuration
 * @return result_t
 */
result_t node_state_bootstrap(node_state_t *state, const roole_config_t *config);

/**
 * Shutdown node
 * Stops all threads, closes connections
 * @param state Node state
 */
void node_state_shutdown(node_state_t *state);

/**
 * Destroy node state
 * Frees all resources
 * @param state Node state
 */
void node_state_destroy(node_state_t *state);

// Accessors (read-only access to internal state)
const node_identity_t* node_state_get_identity(const node_state_t *state);
const node_capabilities_t* node_state_get_capabilities(const node_state_t *state);
datastore_t* node_state_get_datastore(node_state_t *state);
peer_pool_t* node_state_get_peer_pool(node_state_t *state);
cluster_view_t* node_state_get_cluster_view(node_state_t *state);
metrics_registry_t* node_state_get_metrics(node_state_t *state);
event_bus_t* node_state_get_event_bus(node_state_t *state);

// Statistics
typedef struct {
    uint64_t uptime_ms;
    size_t datastore_records;
    size_t datastore_bytes;
    uint64_t datastore_ops_total;
    size_t cluster_size;
} node_statistics_t;

void node_state_get_statistics(const node_state_t *state, node_statistics_t *stats);

#endif // ROOLE_NODE_STATE_H
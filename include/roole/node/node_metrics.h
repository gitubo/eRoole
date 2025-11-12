// include/roole/node/node_metrics.h
// Node metrics initialization and updates

#ifndef ROOLE_NODE_METRICS_H
#define ROOLE_NODE_METRICS_H

#include "roole/node/node_state.h"

/**
 * Initialize metrics system for node
 * Creates registry, registers metrics, starts HTTP server
 * @param state Node state
 * @param metrics_addr Metrics address (ip:port), or NULL to disable
 * @return 0 on success, error code on failure
 */
int node_metrics_init(node_state_t *state, const char *metrics_addr);

/**
 * Shutdown metrics system
 * Stops HTTP server, destroys registry
 * @param state Node state
 */
void node_metrics_shutdown(node_state_t *state);

/**
 * Update periodic metrics
 * Called by metrics update thread
 * @param state Node state
 */
void node_metrics_update_periodic(node_state_t *state);

/**
 * Update cluster metrics
 * Updates cluster member counts
 * @param state Node state
 */
void node_metrics_update_cluster(node_state_t *state);

#endif // ROOLE_NODE_METRICS_H
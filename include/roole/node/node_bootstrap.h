// include/roole/node/node_bootstrap.h
// Node bootstrap (cluster join)

#ifndef ROOLE_NODE_BOOTSTRAP_H
#define ROOLE_NODE_BOOTSTRAP_H

#include "roole/node/node_state.h"
#include "roole/core/config.h"

/**
 * Bootstrap node from configuration
 * Connects to seed routers, joins cluster
 * @param state Node state
 * @param config Configuration
 * @return 0 on success, error code on failure
 */
int node_bootstrap_from_config(node_state_t *state, const roole_config_t *config);

/**
 * Bootstrap with retry
 * Attempts bootstrap multiple times before giving up
 * @param state Node state
 * @param config Configuration
 * @param max_retries Maximum retry attempts
 * @return 0 on success, error code on failure
 */
int node_bootstrap_with_retry(node_state_t *state, const roole_config_t *config, int max_retries);

#endif // ROOLE_NODE_BOOTSTRAP_H
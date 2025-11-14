// include/roole/node/node_capabilities.h
// Node capability detection and management

#ifndef ROOLE_NODE_CAPABILITIES_H
#define ROOLE_NODE_CAPABILITIES_H

#include "roole/config/config.h"
#include "roole/node/node_state.h"

typedef struct node_identity node_identity_t;

// Node capabilities
typedef struct {
    int has_ingress;   // Accepts external client requests
    int can_execute;   // Processes messages (runs executor threads)
    int can_route;     // Routes messages to other nodes
} node_capabilities_t;

/**
 * Detect capabilities from configuration
 * @param config Node configuration
 * @param caps Output capabilities
 * @param identity Output identity
 */
void node_detect_capabilities(const roole_config_t *config,
                              node_capabilities_t *caps,
                              node_identity_t *identity);

/**
 * Print capabilities summary
 * @param caps Capabilities
 * @param identity Identity
 */
void node_print_capabilities(const node_capabilities_t *caps,
                             const node_identity_t *identity);

#endif // ROOLE_NODE_CAPABILITIES_H
// include/roole/node/node_rpc.h
// Node RPC server management

#ifndef ROOLE_NODE_RPC_H
#define ROOLE_NODE_RPC_H

#include "roole/node/node_state.h"
#include "roole/rpc/rpc_server.h"

/**
 * Start RPC servers for node
 * Starts DATA server (always) and INGRESS server (if has_ingress capability)
 * Blocks until servers are stopped
 * @param state Node state
 * @return 0 on clean exit, error code on failure
 */
int node_start_rpc_servers(node_state_t *state);

#endif // ROOLE_NODE_RPC_H
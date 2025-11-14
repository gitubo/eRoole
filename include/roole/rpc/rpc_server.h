// include/roole/rpc/rpc_server.h
// RPC server (event loop, connection management)

#ifndef ROOLE_RPC_SERVER_H
#define ROOLE_RPC_SERVER_H

#include "roole/rpc/rpc_handler.h"
#include "roole/rpc/rpc_channel.h"
#include <stdint.h>

typedef struct rpc_server rpc_server_t;

// Server configuration
typedef struct {
    uint16_t port;
    const char *bind_addr;
    rpc_channel_type_t channel_type;
    size_t max_connections;
    size_t buffer_size;
    int recv_timeout_ms;
} rpc_server_config_t;

/**
 * Create RPC server
 * @param config Server configuration
 * @param handler_registry Handler registry
 * @return Server handle, or NULL on error
 */
rpc_server_t* rpc_server_create(
    const rpc_server_config_t *config,
    rpc_handler_registry_t *handler_registry
);

/**
 * Start RPC server (blocking)
 * Runs event loop - call from dedicated thread
 * Returns when server is stopped
 * @param server Server handle
 * @return 0 on clean exit, -1 on error
 */
int rpc_server_run(rpc_server_t *server);

/**
 * Stop RPC server
 * Signals server to stop - run() will return
 * @param server Server handle
 */
void rpc_server_stop(rpc_server_t *server);

/**
 * Destroy RPC server
 * Closes sockets, frees memory
 * @param server Server handle
 */
void rpc_server_destroy(rpc_server_t *server);

/**
 * Get server statistics
 * @param server Server handle
 * @param out_stats Output statistics
 */
typedef struct {
    uint64_t requests_received;
    uint64_t requests_processed;
    uint64_t requests_failed;
    uint64_t bytes_received;
    uint64_t bytes_sent;
    size_t active_connections;
} rpc_server_stats_t;

void rpc_server_get_stats(rpc_server_t *server, rpc_server_stats_t *out_stats);

#endif // ROOLE_RPC_SERVER_H
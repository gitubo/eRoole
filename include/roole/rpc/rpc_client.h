// include/roole/rpc/rpc_client.h
// RPC client (connection helpers)

#ifndef ROOLE_RPC_CLIENT_H
#define ROOLE_RPC_CLIENT_H

#include "roole/rpc/rpc_types.h"
#include "roole/rpc/rpc_channel.h"
#include <stdint.h>

typedef struct rpc_client rpc_client_t;

/**
 * Create RPC client connection
 * @param remote_ip Remote server IP
 * @param remote_port Remote server port
 * @param channel_type Channel type (DATA or INGRESS)
 * @param buffer_size Buffer size for RX/TX
 * @return Client handle, or NULL on error
 */
rpc_client_t* rpc_client_connect(
    const char *remote_ip,
    uint16_t remote_port,
    rpc_channel_type_t channel_type,
    size_t buffer_size
);

/**
 * Send RPC request (synchronous)
 * Blocks until response is received or timeout
 * @param client Client handle
 * @param func_id Function ID
 * @param request Request payload
 * @param request_len Request length
 * @param response Output response pointer (allocated by function)
 * @param response_len Output response length
 * @param timeout_ms Timeout in milliseconds (0 = no timeout)
 * @return RPC status code, or -1 on network error
 */
int rpc_client_call(
    rpc_client_t *client,
    uint8_t func_id,
    const uint8_t *request,
    size_t request_len,
    uint8_t **response,
    size_t *response_len,
    int timeout_ms
);

/**
 * Send RPC request (asynchronous - fire and forget)
 * Does not wait for response
 * @param client Client handle
 * @param func_id Function ID
 * @param request Request payload
 * @param request_len Request length
 * @return 0 on success, -1 on error
 */
int rpc_client_send_async(
    rpc_client_t *client,
    uint8_t func_id,
    const uint8_t *request,
    size_t request_len
);

/**
 * Close client connection
 * @param client Client handle
 */
void rpc_client_close(rpc_client_t *client);

/**
 * Get channel handle (for advanced use)
 * @param client Client handle
 * @return Channel handle
 */
rpc_channel_t* rpc_client_get_channel(rpc_client_t *client);

#endif // ROOLE_RPC_CLIENT_H
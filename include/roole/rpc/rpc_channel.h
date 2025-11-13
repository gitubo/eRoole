// include/roole/rpc/rpc_channel.h
// RPC channel abstraction (connection management)

#ifndef ROOLE_RPC_CHANNEL_H
#define ROOLE_RPC_CHANNEL_H

#include "roole/rpc/rpc_types.h"
#include <stddef.h>

typedef struct rpc_channel rpc_channel_t;

// Channel type
typedef enum {
    RPC_CHANNEL_DATA = 1,     // Peer-to-peer communication
    RPC_CHANNEL_INGRESS = 2   // Client-facing (Router only)
} rpc_channel_type_t;

typedef struct rpc_channel {
    int socket_fd;
    rpc_channel_type_t channel_type;
    uint8_t *rx_buffer;
    size_t rx_buffer_size;
    size_t rx_data_len;
    uint8_t *tx_buffer;
    size_t tx_buffer_size;
} rpc_channel_t;

/**
 * Initialize RPC channel
 * @param channel Channel structure to initialize
 * @param fd Socket file descriptor
 * @param type Channel type
 * @param buffer_size Buffer size for RX/TX
 * @return 0 on success, -1 on error
 */
int rpc_channel_init(rpc_channel_t *channel, int fd, rpc_channel_type_t type, size_t buffer_size);

/**
 * Destroy RPC channel
 * Closes socket, frees buffers
 * @param channel Channel to destroy
 */
void rpc_channel_destroy(rpc_channel_t *channel);

/**
 * Get channel type
 * @param channel Channel handle
 * @return Channel type
 */
rpc_channel_type_t rpc_channel_get_type(const rpc_channel_t *channel);

/**
 * Get socket file descriptor
 * @param channel Channel handle
 * @return Socket FD
 */
int rpc_channel_get_fd(const rpc_channel_t *channel);

/**
 * Get receive buffer
 * @param channel Channel handle
 * @return Buffer pointer
 */
uint8_t* rpc_channel_get_rx_buffer(rpc_channel_t *channel);

/**
 * Get transmit buffer
 * @param channel Channel handle
 * @return Buffer pointer
 */
uint8_t* rpc_channel_get_tx_buffer(rpc_channel_t *channel);

/**
 * Get receive buffer size
 * @param channel Channel handle
 * @return Buffer size
 */
size_t rpc_channel_get_rx_buffer_size(const rpc_channel_t *channel);

/**
 * Get transmit buffer size
 * @param channel Channel handle
 * @return Buffer size
 */
size_t rpc_channel_get_tx_buffer_size(const rpc_channel_t *channel);

/**
 * Get current data length in RX buffer
 * @param channel Channel handle
 * @return Data length
 */
size_t rpc_channel_get_rx_data_len(const rpc_channel_t *channel);

/**
 * Set data length in RX buffer
 * @param channel Channel handle
 * @param len New data length
 */
void rpc_channel_set_rx_data_len(rpc_channel_t *channel, size_t len);

#endif // ROOLE_RPC_CHANNEL_H
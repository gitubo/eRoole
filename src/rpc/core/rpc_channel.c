#include "roole/rpc/rpc_channel.h"
#include "roole/core/common.h"

int rpc_channel_init(rpc_channel_t *channel, int fd, rpc_channel_type_t type, size_t buffer_size) {
    if (buffer_size == 0) return -1;

    channel->socket_fd = fd;
    channel->channel_type = type;
    channel->rx_buffer_size = buffer_size;
    channel->tx_buffer_size = buffer_size;
    channel->rx_data_len = 0;

    channel->rx_buffer = (uint8_t *)malloc(buffer_size);
    channel->tx_buffer = (uint8_t *)malloc(buffer_size);

    if (!channel->rx_buffer || !channel->tx_buffer) {
        if (channel->rx_buffer) free(channel->rx_buffer);
        if (channel->tx_buffer) free(channel->tx_buffer);
        return -1;
    }

    return 0;
}

void rpc_channel_destroy(rpc_channel_t *channel) {
    if (channel) {
        if (channel->socket_fd != -1) {
            close(channel->socket_fd);
            channel->socket_fd = -1;
        }
        if (channel->rx_buffer) free(channel->rx_buffer);
        if (channel->tx_buffer) free(channel->tx_buffer);
    }
}

rpc_channel_type_t rpc_channel_get_type(const rpc_channel_t *channel) {
    return channel ? channel->type : RPC_CHANNEL_DATA;
}

int rpc_channel_get_fd(const rpc_channel_t *channel) {
    return channel ? channel->socket_fd : -1;
}

uint8_t* rpc_channel_get_rx_buffer(rpc_channel_t *channel) {
    return channel ? channel->rx_buffer : NULL;
}

uint8_t* rpc_channel_get_tx_buffer(rpc_channel_t *channel) {
    return channel ? channel->tx_buffer : NULL;
}

size_t rpc_channel_get_rx_buffer_size(const rpc_channel_t *channel) {
    return channel ? channel->rx_buffer_size : 0;
}

size_t rpc_channel_get_tx_buffer_size(const rpc_channel_t *channel) {
    return channel ? channel->tx_buffer_size : 0;
}

size_t rpc_channel_get_rx_data_len(const rpc_channel_t *channel) {
    return channel ? channel->rx_data_len : 0;
}

void rpc_channel_set_rx_data_len(rpc_channel_t *channel, size_t len) {
    if (channel) channel->rx_data_len = len;
}
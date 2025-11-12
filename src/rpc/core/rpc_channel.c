#include "roole/rpc/rpc_channel.h"
#include "roole/core/common.h"

struct rpc_channel {
    int socket_fd;
    rpc_channel_type_t type;
    uint8_t *rx_buffer;
    uint8_t *tx_buffer;
    size_t rx_buffer_size;
    size_t rx_data_len;
};

int rpc_channel_init(rpc_channel_t *channel, int fd, rpc_channel_type_t type, size_t buffer_size) {
    (void)channel; (void)fd; (void)type; (void)buffer_size;
    LOG_DEBUG("TODO: Implement rpc_channel_init");
    return -1;
}

void rpc_channel_destroy(rpc_channel_t *channel) {
    (void)channel;
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

size_t rpc_channel_get_rx_data_len(const rpc_channel_t *channel) {
    return channel ? channel->rx_data_len : 0;
}

void rpc_channel_set_rx_data_len(rpc_channel_t *channel, size_t len) {
    if (channel) channel->rx_data_len = len;
}
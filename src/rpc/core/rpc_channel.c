// src/rpc/core/rpc_channel.c
// RPC channel implementation with buffer management

#include "roole/rpc/rpc_channel.h"
#include "roole/core/common.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>



int rpc_channel_init(rpc_channel_t *channel, int fd, rpc_channel_type_t type, size_t buffer_size) {
    if (!channel || fd < 0 || buffer_size == 0) {
        return -1;
    }
    
    memset(channel, 0, sizeof(rpc_channel_t));
    
    channel->fd = fd;
    channel->type = type;
    channel->rx_buffer_size = buffer_size;
    channel->tx_buffer_size = buffer_size;
    
    channel->rx_buffer = (uint8_t*)safe_malloc(buffer_size);
    channel->tx_buffer = (uint8_t*)safe_malloc(buffer_size);
    
    if (!channel->rx_buffer || !channel->tx_buffer) {
        safe_free(channel->rx_buffer);
        safe_free(channel->tx_buffer);
        return -1;
    }
    
    channel->rx_data_len = 0;
    channel->is_open = 1;
    
    if (pthread_mutex_init(&channel->lock, NULL) != 0) {
        safe_free(channel->rx_buffer);
        safe_free(channel->tx_buffer);
        return -1;
    }
    
    return 0;
}

void rpc_channel_destroy(rpc_channel_t *channel) {
    if (!channel) return;
    
    pthread_mutex_lock(&channel->lock);
    
    channel->is_open = 0;
    
    if (channel->fd >= 0) {
        close(channel->fd);
        channel->fd = -1;
    }
    
    safe_free(channel->rx_buffer);
    safe_free(channel->tx_buffer);
    
    channel->rx_buffer = NULL;
    channel->tx_buffer = NULL;
    
    pthread_mutex_unlock(&channel->lock);
    pthread_mutex_destroy(&channel->lock);
}

rpc_channel_type_t rpc_channel_get_type(const rpc_channel_t *channel) {
    return channel ? channel->type : 0;
}

int rpc_channel_get_fd(const rpc_channel_t *channel) {
    return channel ? channel->fd : -1;
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
    if (channel && len <= channel->rx_buffer_size) {
        channel->rx_data_len = len;
    }
}
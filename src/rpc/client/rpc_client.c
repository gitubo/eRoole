// src/rpc/client/rpc_client.c
// RPC client with connection management and timeout support

#include "roole/rpc/rpc_client.h"
#include "roole/rpc/rpc_channel.h"
#include "roole/rpc/rpc_types.h"
#include "roole/core/common.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <sys/select.h>
#include <fcntl.h>
#include <pthread.h>

struct rpc_client {
    char remote_ip[16];
    uint16_t remote_port;
    rpc_channel_t channel;
    int connected;
    uint32_t next_request_id;
    node_id_t local_node_id;
    pthread_mutex_t lock;
};

// ============================================================================
// Helper Functions
// ============================================================================

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int set_blocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
}

static int connect_with_timeout(int sockfd, const struct sockaddr *addr, 
                                socklen_t addrlen, int timeout_ms) {
    // Set non-blocking
    if (set_nonblocking(sockfd) < 0) {
        LOG_ERROR("Failed to set non-blocking: %s", strerror(errno));
        return -1;
    }
    
    int ret = connect(sockfd, addr, addrlen);
    if (ret == 0) {
        // Connected immediately (unlikely)
        set_blocking(sockfd);
        return 0;
    }
    
    if (errno != EINPROGRESS) {
        LOG_ERROR("connect() failed immediately: %s", strerror(errno));
        return -1;
    }
    
    // Wait for connection with timeout
    fd_set write_fds;
    FD_ZERO(&write_fds);
    FD_SET(sockfd, &write_fds);
    
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    
    ret = select(sockfd + 1, NULL, &write_fds, NULL, &tv);
    if (ret <= 0) {
        if (ret == 0) {
            LOG_ERROR("Connection timeout after %dms", timeout_ms);
        } else {
            LOG_ERROR("select() failed: %s", strerror(errno));
        }
        return -1;
    }
    
    // Check if connection succeeded
    int error = 0;
    socklen_t len = sizeof(error);
    if (getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &error, &len) < 0) {
        LOG_ERROR("getsockopt() failed: %s", strerror(errno));
        return -1;
    }
    
    if (error != 0) {
        LOG_ERROR("Connection failed: %s", strerror(error));
        errno = error;
        return -1;
    }
    
    // Set back to blocking mode for easier send/recv
    set_blocking(sockfd);
    
    return 0;
}

static int recv_exact(int sockfd, uint8_t *buffer, size_t n, int timeout_ms) {
    size_t total_received = 0;
    
    while (total_received < n) {
        if (timeout_ms > 0) {
            fd_set read_fds;
            FD_ZERO(&read_fds);
            FD_SET(sockfd, &read_fds);
            
            struct timeval tv;
            tv.tv_sec = timeout_ms / 1000;
            tv.tv_usec = (timeout_ms % 1000) * 1000;
            
            int ret = select(sockfd + 1, &read_fds, NULL, NULL, &tv);
            if (ret < 0) {
                LOG_ERROR("select() failed: %s", strerror(errno));
                return -1;
            }
            if (ret == 0) {
                LOG_WARN("recv_exact timeout after %dms", timeout_ms);
                return -1;  // Timeout
            }
        }
        
        ssize_t received = recv(sockfd, buffer + total_received, 
                               n - total_received, 0);
        if (received <= 0) {
            if (received == 0) {
                LOG_WARN("Connection closed by peer during recv");
            } else {
                LOG_ERROR("recv() failed: %s", strerror(errno));
            }
            return -1;
        }
        
        total_received += received;
    }
    
    return 0;
}

// ============================================================================
// Public API
// ============================================================================

rpc_client_t* rpc_client_connect(const char *remote_ip, uint16_t remote_port,
                                 rpc_channel_type_t channel_type, size_t buffer_size) {
    if (!remote_ip || remote_port == 0 || buffer_size == 0) {
        LOG_ERROR("Invalid client parameters");
        return NULL;
    }
    
    LOG_INFO("Connecting to RPC server: %s:%u", remote_ip, remote_port);
    
    rpc_client_t *client = (rpc_client_t*)safe_calloc(1, sizeof(rpc_client_t));
    if (!client) {
        LOG_ERROR("Failed to allocate client");
        return NULL;
    }
    
    safe_strncpy(client->remote_ip, remote_ip, sizeof(client->remote_ip));
    client->remote_port = remote_port;
    client->connected = 0;
    client->next_request_id = 1;
    client->local_node_id = 0;  // TODO: Get from config/context
    
    if (pthread_mutex_init(&client->lock, NULL) != 0) {
        LOG_ERROR("Failed to initialize client mutex");
        safe_free(client);
        return NULL;
    }
    
    // Create socket
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        LOG_ERROR("Failed to create socket: %s", strerror(errno));
        pthread_mutex_destroy(&client->lock);
        safe_free(client);
        return NULL;
    }
    
    // Set TCP_NODELAY for low latency
    int flag = 1;
    setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
    
    // Set keep-alive
    setsockopt(sockfd, SOL_SOCKET, SO_KEEPALIVE, &flag, sizeof(flag));
    
    // Connect to server
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(remote_port);
    
    if (inet_pton(AF_INET, remote_ip, &server_addr.sin_addr) <= 0) {
        LOG_ERROR("Invalid IP address: %s", remote_ip);
        close(sockfd);
        pthread_mutex_destroy(&client->lock);
        safe_free(client);
        return NULL;
    }
    
    if (connect_with_timeout(sockfd, (struct sockaddr*)&server_addr, 
                            sizeof(server_addr), 5000) < 0) {
        LOG_ERROR("Failed to connect to %s:%u", remote_ip, remote_port);
        close(sockfd);
        pthread_mutex_destroy(&client->lock);
        safe_free(client);
        return NULL;
    }
    
    // Initialize channel
    if (rpc_channel_init(&client->channel, sockfd, channel_type, buffer_size) < 0) {
        LOG_ERROR("Failed to initialize RPC channel");
        close(sockfd);
        pthread_mutex_destroy(&client->lock);
        safe_free(client);
        return NULL;
    }
    
    client->connected = 1;
    
    LOG_INFO("RPC client connected to %s:%u", remote_ip, remote_port);
    return client;
}

int rpc_client_call(rpc_client_t *client, uint8_t func_id,
                   const uint8_t *request, size_t request_len,
                   uint8_t **response, size_t *response_len, int timeout_ms) {
    if (!client || !client->connected || !response || !response_len) {
        LOG_ERROR("Invalid client call arguments");
        return RPC_STATUS_INTERNAL_ERROR;
    }
    
    *response = NULL;
    *response_len = 0;
    
    pthread_mutex_lock(&client->lock);
    
    // Generate request ID
    uint32_t request_id = client->next_request_id++;
    
    // Pack message
    uint8_t *tx_buffer = rpc_channel_get_tx_buffer(&client->channel);
    size_t tx_buffer_size = rpc_channel_get_tx_buffer_size(&client->channel);
    
    if (request_len + RPC_HEADER_SIZE > tx_buffer_size) {
        LOG_ERROR("Request too large: %zu + %d > %zu", 
                 request_len, RPC_HEADER_SIZE, tx_buffer_size);
        pthread_mutex_unlock(&client->lock);
        return RPC_STATUS_BAD_ARGUMENT;
    }
    
    size_t msg_len = rpc_pack_message(tx_buffer, client->local_node_id, request_id,
                                      RPC_TYPE_REQUEST, RPC_STATUS_SUCCESS, func_id,
                                      request, request_len);
    
    // Send request
    int sockfd = rpc_channel_get_fd(&client->channel);
    ssize_t sent = send(sockfd, tx_buffer, msg_len, 0);
    
    pthread_mutex_unlock(&client->lock);
    
    if (sent != (ssize_t)msg_len) {
        LOG_ERROR("Failed to send complete message: sent=%zd, expected=%zu: %s", 
                 sent, msg_len, strerror(errno));
        return RPC_STATUS_NETWORK;
    }
    
    LOG_DEBUG("Sent RPC request: func_id=%u, req_id=%u, len=%zu", 
             func_id, request_id, msg_len);
    
    // Receive response header
    uint8_t header_buf[RPC_HEADER_SIZE];
    if (recv_exact(sockfd, header_buf, RPC_HEADER_SIZE, timeout_ms) < 0) {
        LOG_ERROR("Failed to receive response header");
        return RPC_STATUS_TIMEOUT;
    }
    
    rpc_header_t header;
    if (rpc_unpack_header(header_buf, &header) < 0) {
        LOG_ERROR("Failed to unpack response header");
        return RPC_STATUS_NETWORK;
    }
    
    // Validate response
    if (header.request_id != request_id) {
        LOG_ERROR("Request ID mismatch: expected=%u, got=%u", 
                 request_id, header.request_id);
        return RPC_STATUS_NETWORK;
    }
    
    if (header.type_and_status.fields.type != RPC_TYPE_RESPONSE) {
        LOG_ERROR("Invalid response type: %u", 
                 header.type_and_status.fields.type);
        return RPC_STATUS_NETWORK;
    }
    
    uint8_t status = header.type_and_status.fields.status;
    
    LOG_DEBUG("Received response header: status=%u, total_len=%u", 
             status, header.total_len);
    
    // Receive payload if present
    size_t payload_len = header.total_len - RPC_HEADER_SIZE;
    
    if (payload_len > 0) {
        *response = (uint8_t*)safe_malloc(payload_len);
        if (!*response) {
            LOG_ERROR("Failed to allocate response buffer");
            return RPC_STATUS_INTERNAL_ERROR;
        }
        
        if (recv_exact(sockfd, *response, payload_len, timeout_ms) < 0) {
            LOG_ERROR("Failed to receive response payload");
            safe_free(*response);
            *response = NULL;
            return RPC_STATUS_TIMEOUT;
        }
        
        *response_len = payload_len;
        LOG_DEBUG("Received response payload: %zu bytes", payload_len);
    }
    
    LOG_DEBUG("RPC call completed: status=%u", status);
    
    return status;
}

int rpc_client_send_async(rpc_client_t *client, uint8_t func_id,
                          const uint8_t *request, size_t request_len) {
    if (!client || !client->connected) {
        LOG_ERROR("Invalid client or not connected");
        return -1;
    }
    
    pthread_mutex_lock(&client->lock);
    
    uint32_t request_id = client->next_request_id++;
    
    uint8_t *tx_buffer = rpc_channel_get_tx_buffer(&client->channel);
    size_t tx_buffer_size = rpc_channel_get_tx_buffer_size(&client->channel);
    
    if (request_len + RPC_HEADER_SIZE > tx_buffer_size) {
        LOG_ERROR("Request too large for async send");
        pthread_mutex_unlock(&client->lock);
        return -1;
    }
    
    size_t msg_len = rpc_pack_message(tx_buffer, client->local_node_id, request_id,
                                      RPC_TYPE_REQUEST, RPC_STATUS_SUCCESS, func_id,
                                      request, request_len);
    
    int sockfd = rpc_channel_get_fd(&client->channel);
    ssize_t sent = send(sockfd, tx_buffer, msg_len, 0);
    
    pthread_mutex_unlock(&client->lock);
    
    if (sent != (ssize_t)msg_len) {
        LOG_ERROR("Failed to send async message: %s", strerror(errno));
        return -1;
    }
    
    LOG_DEBUG("Sent async RPC: func_id=%u, req_id=%u, len=%zu", 
             func_id, request_id, msg_len);
    return 0;
}

void rpc_client_close(rpc_client_t *client) {
    if (!client) return;
    
    if (client->connected) {
        LOG_INFO("Closing RPC client connection to %s:%u", 
                client->remote_ip, client->remote_port);
        
        rpc_channel_destroy(&client->channel);
        client->connected = 0;
    }
    
    pthread_mutex_destroy(&client->lock);
    safe_free(client);
}

rpc_channel_t* rpc_client_get_channel(rpc_client_t *client) {
    return client ? &client->channel : NULL;
}
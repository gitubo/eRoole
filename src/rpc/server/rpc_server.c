// src/rpc/server/rpc_server.c
// High-performance RPC server with epoll event loop

#include "roole/rpc/rpc_server.h"
#include "roole/rpc/rpc_channel.h"
#include "roole/rpc/rpc_types.h"
#include "roole/core/common.h"
#include "roole/logger/logger.h"
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>

#define MAX_EPOLL_EVENTS 128
#define LISTEN_BACKLOG 512

typedef struct connection_state {
    int fd;
    rpc_channel_t channel;
    uint64_t last_activity_ms;
    int active;
} connection_state_t;

struct rpc_server {
    int listen_fd;
    int epoll_fd;
    rpc_server_config_t config;
    rpc_handler_registry_t *handler_registry;
    
    connection_state_t *connections;
    size_t max_connections;
    
    volatile int running;
    
    // Statistics (atomic for thread safety)
    _Atomic uint64_t requests_received;
    _Atomic uint64_t requests_processed;
    _Atomic uint64_t requests_failed;
    _Atomic uint64_t bytes_received;
    _Atomic uint64_t bytes_sent;
    _Atomic size_t active_connections;
    
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

static connection_state_t* find_free_connection(rpc_server_t *server) {
    for (size_t i = 0; i < server->max_connections; i++) {
        if (!server->connections[i].active) {
            return &server->connections[i];
        }
    }
    return NULL;
}

static void close_connection(rpc_server_t *server, connection_state_t *conn) {
    if (!conn || !conn->active) return;
    
    epoll_ctl(server->epoll_fd, EPOLL_CTL_DEL, conn->fd, NULL);
    rpc_channel_destroy(&conn->channel);
    conn->active = 0;
    server->active_connections--;
    
    LOG_DEBUG("Connection closed: fd=%d (active=%zu)", conn->fd, 
              (size_t)server->active_connections);
}

// ============================================================================
// Request Processing
// ============================================================================

static void process_request(rpc_server_t *server, connection_state_t *conn,
                           const rpc_header_t *header, const uint8_t *payload) {
    server->requests_received++;
    
    LOG_DEBUG("Processing request: func_id=%u, req_id=%u, sender=%u",
              header->func_id, header->request_id, header->sender_id);
    
    // Lookup handler
    void *user_context = NULL;
    rpc_handler_fn handler = rpc_handler_lookup(server->handler_registry,
                                                header->func_id, &user_context);
    
    uint8_t *response_payload = NULL;
    size_t response_len = 0;
    uint8_t status = RPC_STATUS_SUCCESS;
    
    if (!handler) {
        LOG_WARN("No handler for func_id=%u", header->func_id);
        status = RPC_STATUS_FUNC_NOT_FOUND;
    } else {
        // Calculate payload length
        size_t payload_len = header->total_len - RPC_HEADER_SIZE;
        
        // Invoke handler
        status = handler(payload, payload_len,
                        &response_payload, &response_len, user_context);
        
        if (status == RPC_STATUS_SUCCESS) {
            server->requests_processed++;
            LOG_DEBUG("Handler succeeded: func_id=%u, response_len=%zu",
                     header->func_id, response_len);
        } else {
            server->requests_failed++;
            LOG_WARN("Handler failed: func_id=%u, status=%u", 
                    header->func_id, status);
        }
    }
    
    // Send response
    uint8_t *tx_buffer = rpc_channel_get_tx_buffer(&conn->channel);
    size_t tx_buffer_size = rpc_channel_get_tx_buffer_size(&conn->channel);
    
    size_t response_msg_len = rpc_pack_message(tx_buffer, 0, header->request_id,
                                               RPC_TYPE_RESPONSE, status,
                                               header->func_id,
                                               response_payload, response_len);
    
    if (response_msg_len > tx_buffer_size) {
        LOG_ERROR("Response too large: %zu > %zu", response_msg_len, tx_buffer_size);
        // Try to send error response without payload
        response_msg_len = rpc_pack_message(tx_buffer, 0, header->request_id,
                                           RPC_TYPE_RESPONSE, RPC_STATUS_INTERNAL_ERROR,
                                           header->func_id, NULL, 0);
    }
    
    ssize_t sent = send(conn->fd, tx_buffer, response_msg_len, MSG_NOSIGNAL);
    if (sent > 0) {
        server->bytes_sent += sent;
        LOG_DEBUG("Response sent: %zd bytes", sent);
    } else {
        LOG_ERROR("Failed to send response: %s", strerror(errno));
    }
    
    // Free response payload
    if (response_payload) {
        safe_free(response_payload);
    }
}

// ============================================================================
// Connection Handling
// ============================================================================

static void handle_connection_data(rpc_server_t *server, connection_state_t *conn) {
    uint8_t *rx_buffer = rpc_channel_get_rx_buffer(&conn->channel);
    size_t rx_buffer_size = rpc_channel_get_rx_buffer_size(&conn->channel);
    size_t rx_data_len = rpc_channel_get_rx_data_len(&conn->channel);
    
    // Receive data
    ssize_t received = recv(conn->fd, rx_buffer + rx_data_len,
                           rx_buffer_size - rx_data_len, 0);
    
    if (received <= 0) {
        if (received == 0) {
            LOG_DEBUG("Connection closed by peer: fd=%d", conn->fd);
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            LOG_ERROR("recv() failed: %s", strerror(errno));
        }
        close_connection(server, conn);
        return;
    }
    
    server->bytes_received += received;
    rx_data_len += received;
    rpc_channel_set_rx_data_len(&conn->channel, rx_data_len);
    conn->last_activity_ms = time_now_ms();
    
    LOG_DEBUG("Received %zd bytes, total in buffer: %zu", received, rx_data_len);
    
    // Process complete messages
    while (rx_data_len >= RPC_HEADER_SIZE) {
        rpc_header_t header;
        if (rpc_unpack_header(rx_buffer, &header) < 0) {
            LOG_ERROR("Invalid RPC header, closing connection");
            close_connection(server, conn);
            return;
        }
        
        // Validate header
        if (header.total_len < RPC_HEADER_SIZE || header.total_len > rx_buffer_size) {
            LOG_ERROR("Invalid message length: %u (buffer size: %zu)", 
                     header.total_len, rx_buffer_size);
            close_connection(server, conn);
            return;
        }
        
        // Check if we have complete message
        if (rx_data_len < header.total_len) {
            LOG_DEBUG("Incomplete message: have %zu, need %u", 
                     rx_data_len, header.total_len);
            break;  // Need more data
        }
        
        // Extract payload
        const uint8_t *payload = (header.total_len > RPC_HEADER_SIZE) ?
                                 (rx_buffer + RPC_HEADER_SIZE) : NULL;
        
        // Process request
        process_request(server, conn, &header, payload);
        
        // Remove processed message from buffer
        size_t remaining = rx_data_len - header.total_len;
        if (remaining > 0) {
            memmove(rx_buffer, rx_buffer + header.total_len, remaining);
        }
        rx_data_len = remaining;
        rpc_channel_set_rx_data_len(&conn->channel, rx_data_len);
        
        LOG_DEBUG("Message processed, remaining in buffer: %zu", rx_data_len);
    }
}

static void accept_connection(rpc_server_t *server) {
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    
    int client_fd = accept(server->listen_fd, (struct sockaddr*)&client_addr, &addr_len);
    if (client_fd < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            LOG_ERROR("accept() failed: %s", strerror(errno));
        }
        return;
    }
    
    // Set non-blocking
    if (set_nonblocking(client_fd) < 0) {
        LOG_ERROR("Failed to set client socket non-blocking");
        close(client_fd);
        return;
    }
    
    // Set TCP_NODELAY for low latency
    int flag = 1;
    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
    
    // Find free connection slot
    connection_state_t *conn = find_free_connection(server);
    if (!conn) {
        LOG_WARN("Max connections reached (%zu), rejecting client", 
                server->max_connections);
        close(client_fd);
        return;
    }
    
    // Initialize connection
    if (rpc_channel_init(&conn->channel, client_fd, server->config.channel_type,
                        server->config.buffer_size) < 0) {
        LOG_ERROR("Failed to initialize channel");
        close(client_fd);
        return;
    }
    
    conn->fd = client_fd;
    conn->last_activity_ms = time_now_ms();
    conn->active = 1;
    
    // Add to epoll
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET;  // Edge-triggered
    ev.data.ptr = conn;
    
    if (epoll_ctl(server->epoll_fd, EPOLL_CTL_ADD, client_fd, &ev) < 0) {
        LOG_ERROR("epoll_ctl(ADD) failed: %s", strerror(errno));
        rpc_channel_destroy(&conn->channel);
        conn->active = 0;
        return;
    }
    
    server->active_connections++;
    
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));
    LOG_INFO("New connection: fd=%d, from=%s:%u (active=%zu)", 
             client_fd, ip, ntohs(client_addr.sin_port),
             (size_t)server->active_connections);
}

// ============================================================================
// Public API
// ============================================================================

rpc_server_t* rpc_server_create(const rpc_server_config_t *config,
                                rpc_handler_registry_t *handler_registry) {
    if (!config || !handler_registry) {
        LOG_ERROR("Invalid server parameters");
        return NULL;
    }
    
    if (config->max_connections == 0 || config->buffer_size == 0) {
        LOG_ERROR("Invalid configuration: max_connections=%zu, buffer_size=%zu",
                 config->max_connections, config->buffer_size);
        return NULL;
    }
    
    rpc_server_t *server = (rpc_server_t*)safe_calloc(1, sizeof(rpc_server_t));
    if (!server) {
        LOG_ERROR("Failed to allocate server");
        return NULL;
    }
    
    server->config = *config;
    server->handler_registry = handler_registry;
    server->max_connections = config->max_connections;
    server->running = 0;
    server->listen_fd = -1;
    server->epoll_fd = -1;
    
    pthread_mutex_init(&server->lock, NULL);
    
    // Allocate connection pool
    server->connections = (connection_state_t*)safe_calloc(server->max_connections,
                                                           sizeof(connection_state_t));
    if (!server->connections) {
        LOG_ERROR("Failed to allocate connection pool");
        pthread_mutex_destroy(&server->lock);
        safe_free(server);
        return NULL;
    }
    
    // Create listen socket
    server->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server->listen_fd < 0) {
        LOG_ERROR("Failed to create socket: %s", strerror(errno));
        safe_free(server->connections);
        pthread_mutex_destroy(&server->lock);
        safe_free(server);
        return NULL;
    }
    
    // Set socket options
    int reuse = 1;
    setsockopt(server->listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    setsockopt(server->listen_fd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
    
    if (set_nonblocking(server->listen_fd) < 0) {
        LOG_ERROR("Failed to set listen socket non-blocking");
        close(server->listen_fd);
        safe_free(server->connections);
        pthread_mutex_destroy(&server->lock);
        safe_free(server);
        return NULL;
    }
    
    // Bind
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(config->port);
    
    if (inet_pton(AF_INET, config->bind_addr, &server_addr.sin_addr) <= 0) {
        LOG_ERROR("Invalid bind address: %s", config->bind_addr);
        close(server->listen_fd);
        safe_free(server->connections);
        pthread_mutex_destroy(&server->lock);
        safe_free(server);
        return NULL;
    }
    
    if (bind(server->listen_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        LOG_ERROR("bind() failed on %s:%u: %s", 
                 config->bind_addr, config->port, strerror(errno));
        close(server->listen_fd);
        safe_free(server->connections);
        pthread_mutex_destroy(&server->lock);
        safe_free(server);
        return NULL;
    }
    
    // Listen
    if (listen(server->listen_fd, LISTEN_BACKLOG) < 0) {
        LOG_ERROR("listen() failed: %s", strerror(errno));
        close(server->listen_fd);
        safe_free(server->connections);
        pthread_mutex_destroy(&server->lock);
        safe_free(server);
        return NULL;
    }
    
    // Create epoll
    server->epoll_fd = epoll_create1(0);
    if (server->epoll_fd < 0) {
        LOG_ERROR("epoll_create1() failed: %s", strerror(errno));
        close(server->listen_fd);
        safe_free(server->connections);
        pthread_mutex_destroy(&server->lock);
        safe_free(server);
        return NULL;
    }
    
    // Add listen socket to epoll
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.ptr = NULL;  // NULL means listen socket
    
    if (epoll_ctl(server->epoll_fd, EPOLL_CTL_ADD, server->listen_fd, &ev) < 0) {
        LOG_ERROR("epoll_ctl(ADD listen) failed: %s", strerror(errno));
        close(server->epoll_fd);
        close(server->listen_fd);
        safe_free(server->connections);
        pthread_mutex_destroy(&server->lock);
        safe_free(server);
        return NULL;
    }
    
    LOG_INFO("RPC server created: %s:%u (max_conn=%zu, buffer=%zu)",
             config->bind_addr, config->port, 
             config->max_connections, config->buffer_size);
    
    return server;
}

int rpc_server_run(rpc_server_t *server) {
    if (!server) {
        LOG_ERROR("NULL server");
        return -1;
    }
    
    server->running = 1;
    
    LOG_INFO("RPC server event loop started");
    
    struct epoll_event events[MAX_EPOLL_EVENTS];
    
    while (server->running) {
        int nfds = epoll_wait(server->epoll_fd, events, MAX_EPOLL_EVENTS, 100);
        
        if (nfds < 0) {
            if (errno == EINTR) continue;
            LOG_ERROR("epoll_wait() failed: %s", strerror(errno));
            break;
        }
        
        for (int i = 0; i < nfds; i++) {
            if (events[i].data.ptr == NULL) {
                // Listen socket - accept new connection
                accept_connection(server);
            } else {
                // Client connection - handle data
                connection_state_t *conn = (connection_state_t*)events[i].data.ptr;
                
                if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                    LOG_DEBUG("Connection error/hangup: fd=%d", conn->fd);
                    close_connection(server, conn);
                } else if (events[i].events & EPOLLIN) {
                    handle_connection_data(server, conn);
                }
            }
        }
    }
    
    LOG_INFO("RPC server event loop stopped");
    return 0;
}

void rpc_server_stop(rpc_server_t *server) {
    if (!server) return;
    
    LOG_INFO("Stopping RPC server...");
    server->running = 0;
}

void rpc_server_destroy(rpc_server_t *server) {
    if (!server) return;
    
    LOG_INFO("Destroying RPC server...");
    
    // Close all connections
    for (size_t i = 0; i < server->max_connections; i++) {
        if (server->connections[i].active) {
            close_connection(server, &server->connections[i]);
        }
    }
    
    if (server->epoll_fd >= 0) {
        close(server->epoll_fd);
    }
    
    if (server->listen_fd >= 0) {
        close(server->listen_fd);
    }
    
    pthread_mutex_destroy(&server->lock);
    safe_free(server->connections);
    safe_free(server);
    
    LOG_INFO("RPC server destroyed");
}

void rpc_server_get_stats(rpc_server_t *server, rpc_server_stats_t *out_stats) {
    if (!server || !out_stats) return;
    
    memset(out_stats, 0, sizeof(rpc_server_stats_t));
    
    out_stats->requests_received = server->requests_received;
    out_stats->requests_processed = server->requests_processed;
    out_stats->requests_failed = server->requests_failed;
    out_stats->bytes_received = server->bytes_received;
    out_stats->bytes_sent = server->bytes_sent;
    out_stats->active_connections = server->active_connections;
}
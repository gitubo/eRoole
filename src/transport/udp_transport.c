// src/transport/udp_transport.c
#define _POSIX_C_SOURCE 200809L

#include "roole/transport/udp_transport.h"
#include "roole/logger/logger.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>

typedef struct {
    transport_t base;
    int socket_fd;
    pthread_t receiver_thread;
    transport_recv_callback_fn recv_callback;
    void *user_data;
    udp_transport_stats_t stats;
} udp_transport_impl_t;

static void* udp_receiver_thread(void *arg) {
    udp_transport_impl_t *udp = (udp_transport_impl_t*)arg;
    uint8_t buffer[65536];
    char src_ip[16];
    uint16_t src_port;
    
    while (!udp->base.shutdown_flag) {
        struct sockaddr_in src_addr;
        socklen_t src_len = sizeof(src_addr);
        
        ssize_t received = recvfrom(udp->socket_fd, buffer, sizeof(buffer), 0,
                                   (struct sockaddr*)&src_addr, &src_len);
        
        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(10000); // 10ms
                continue;
            }
            LOG_ERROR("UDP recvfrom failed: %s", strerror(errno));
            continue;
        }
        
        inet_ntop(AF_INET, &src_addr.sin_addr, src_ip, sizeof(src_ip));
        src_port = ntohs(src_addr.sin_port);
        
        udp->stats.bytes_received += received;
        udp->stats.packets_received++;
        
        if (udp->recv_callback) {
            udp->recv_callback(buffer, received, src_ip, src_port, udp->user_data);
        }
    }
    
    return NULL;
}

udp_transport_t* udp_transport_create(const char *bind_addr, uint16_t port) {
    udp_transport_impl_t *udp = calloc(1, sizeof(udp_transport_impl_t));
    if (!udp) return NULL;
    
    udp->base.type = TRANSPORT_TYPE_UDP;
    safe_strncpy(udp->base.bind_addr, bind_addr ? bind_addr : "0.0.0.0", 16);
    udp->base.port = port;
    udp->base.shutdown_flag = 0;
    
    // Create socket
    udp->socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp->socket_fd < 0) {
        LOG_ERROR("Failed to create UDP socket: %s", strerror(errno));
        free(udp);
        return NULL;
    }
    
    // Set non-blocking
    int flags = fcntl(udp->socket_fd, F_GETFL, 0);
    if (flags == -1 || fcntl(udp->socket_fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        LOG_WARN("Failed to set non-blocking mode: %s", strerror(errno));
    }
    
    // Set reuseaddr
    int reuse = 1;
    if (setsockopt(udp->socket_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        LOG_WARN("Failed to set SO_REUSEADDR: %s", strerror(errno));
    }
    
    // Bind
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    
    if (bind_addr && strcmp(bind_addr, "0.0.0.0") != 0) {
        if (inet_pton(AF_INET, bind_addr, &addr.sin_addr) != 1) {
            LOG_ERROR("Invalid bind address: %s", bind_addr);
            close(udp->socket_fd);
            free(udp);
            return NULL;
        }
    } else {
        addr.sin_addr.s_addr = INADDR_ANY;
    }
    
    if (bind(udp->socket_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("Failed to bind UDP socket: %s", strerror(errno));
        close(udp->socket_fd);
        free(udp);
        return NULL;
    }
    
    LOG_INFO("UDP transport created on %s:%u", udp->base.bind_addr, port);
    return (udp_transport_t*)udp;
}

int udp_transport_send(udp_transport_t *transport, const uint8_t *data, size_t len,
                       const char *dest_ip, uint16_t dest_port) {
    udp_transport_impl_t *udp = (udp_transport_impl_t*)transport;
    
    struct sockaddr_in dest_addr;
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(dest_port);
    
    if (inet_pton(AF_INET, dest_ip, &dest_addr.sin_addr) != 1) {
        LOG_ERROR("Invalid destination IP: %s", dest_ip);
        udp->stats.send_errors++;
        return -1;
    }
    
    ssize_t sent = sendto(udp->socket_fd, data, len, 0,
                         (struct sockaddr*)&dest_addr, sizeof(dest_addr));
    
    if (sent < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            LOG_ERROR("UDP sendto failed: %s", strerror(errno));
        }
        udp->stats.send_errors++;
        return -1;
    }
    
    udp->stats.bytes_sent += sent;
    udp->stats.packets_sent++;
    return sent;
}

int udp_transport_start_receiver(udp_transport_t *transport,
                                transport_recv_callback_fn callback,
                                void *user_data) {
    udp_transport_impl_t *udp = (udp_transport_impl_t*)transport;
    
    udp->recv_callback = callback;
    udp->user_data = user_data;
    
    if (pthread_create(&udp->receiver_thread, NULL, udp_receiver_thread, udp) != 0) {
        LOG_ERROR("Failed to create UDP receiver thread");
        return -1;
    }
    
    return 0;
}

void udp_transport_stop_receiver(udp_transport_t *transport) {
if (!transport) return;

    udp_transport_impl_t *udp = (udp_transport_impl_t*)transport;

    // 1. Set the shutdown flag to signal the receiver thread to exit its loop
    udp->base.shutdown_flag = 1;

    // 2. The existing code uses recvfrom with O_NONBLOCK, which means the thread 
    //    will exit after a maximum of 10ms (usleep(10000)) once the flag is set.
    //    We wait for the thread to terminate gracefully.
    if (udp->receiver_thread) {
        LOG_DEBUG("Stopping UDP receiver thread on %s:%u", udp->base.bind_addr, udp->base.port);
        pthread_join(udp->receiver_thread, NULL);
        udp->receiver_thread = 0; // Clear the thread handle
        LOG_DEBUG("UDP receiver thread stopped");
    }
}

void udp_transport_destroy(udp_transport_t *transport) {
    if (!transport) return;
    
    udp_transport_impl_t *udp = (udp_transport_impl_t*)transport;
    
    udp->base.shutdown_flag = 1;
    
    if (udp->receiver_thread) {
        pthread_join(udp->receiver_thread, NULL);
    }
    
    if (udp->socket_fd >= 0) {
        close(udp->socket_fd);
    }
    
    free(udp);
    LOG_INFO("UDP transport destroyed");
}

void udp_transport_get_stats(udp_transport_t *transport, udp_transport_stats_t *out_stats) {
    if (!transport || !out_stats) return;
    
    udp_transport_impl_t *udp = (udp_transport_impl_t*)transport;
    *out_stats = udp->stats;
}
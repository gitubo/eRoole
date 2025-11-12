// include/roole/transport/udp_transport.h
// UDP-specific transport implementation

#ifndef ROOLE_UDP_TRANSPORT_H
#define ROOLE_UDP_TRANSPORT_H

#include "roole/transport/transport.h"
#include <pthread.h>

typedef struct udp_transport udp_transport_t;

/**
 * Create UDP transport
 * @param bind_addr IP address to bind (e.g., "0.0.0.0")
 * @param port Port to bind
 * @return Transport handle, or NULL on error
 */
udp_transport_t* udp_transport_create(const char *bind_addr, uint16_t port);

/**
 * Send UDP datagram
 * Thread-safe: can be called from multiple threads
 * @param transport Transport handle
 * @param data Payload data
 * @param len Payload length
 * @param dest_ip Destination IP
 * @param dest_port Destination port
 * @return 0 on success, -1 on error
 */
int udp_transport_send(udp_transport_t *transport, const uint8_t *data, size_t len,
                       const char *dest_ip, uint16_t dest_port);

/**
 * Start receiver thread
 * Spawns a background thread that calls callback for each received message
 * @param transport Transport handle
 * @param callback Callback invoked for each message
 * @param user_data User data passed to callback
 * @return 0 on success, -1 on error
 */
int udp_transport_start_receiver(udp_transport_t *transport,
                                 transport_recv_callback_fn callback,
                                 void *user_data);

/**
 * Stop receiver thread
 * Blocks until receiver thread exits
 * @param transport Transport handle
 */
void udp_transport_stop_receiver(udp_transport_t *transport);

/**
 * Destroy UDP transport
 * Stops receiver if running, closes socket, frees memory
 * @param transport Transport handle
 */
void udp_transport_destroy(udp_transport_t *transport);

/**
 * Get transport statistics
 * @param transport Transport handle
 * @param out_stats Output statistics structure
 */
typedef struct {
    uint64_t bytes_sent;
    uint64_t bytes_received;
    uint64_t packets_sent;
    uint64_t packets_received;
    uint64_t send_errors;
    uint64_t recv_errors;
} udp_transport_stats_t;

void udp_transport_get_stats(udp_transport_t *transport, udp_transport_stats_t *out_stats);

#endif // ROOLE_UDP_TRANSPORT_H
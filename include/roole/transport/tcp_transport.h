// include/roole/transport/tcp_transport.h
// TCP-specific transport (for RPC)

#ifndef ROOLE_TCP_TRANSPORT_H
#define ROOLE_TCP_TRANSPORT_H

#include "roole/transport/transport.h"
#include <stdint.h>

typedef struct tcp_transport tcp_transport_t;
typedef struct tcp_connection tcp_connection_t;

// Connection state callback
typedef void (*tcp_connection_callback_fn)(
    tcp_connection_t *conn,
    const uint8_t *data,
    size_t len,
    void *user_data
);

/**
 * Create TCP transport (server mode)
 * @param bind_addr IP to bind
 * @param port Port to listen on
 * @param max_connections Maximum concurrent connections
 * @return Transport handle, or NULL on error
 */
tcp_transport_t* tcp_transport_create_server(const char *bind_addr, uint16_t port,
                                             size_t max_connections);

/**
 * Create TCP connection (client mode)
 * @param remote_ip Remote server IP
 * @param remote_port Remote server port
 * @return Connection handle, or NULL on error
 */
tcp_connection_t* tcp_transport_connect(const char *remote_ip, uint16_t remote_port);

/**
 * Start accepting connections (server mode)
 * @param transport Transport handle
 * @param on_accept Callback invoked for each new connection
 * @param user_data User data passed to callback
 * @return 0 on success, -1 on error
 */
int tcp_transport_start_accepting(tcp_transport_t *transport,
                                  tcp_connection_callback_fn on_accept,
                                  void *user_data);

/**
 * Send data on TCP connection
 * @param conn Connection handle
 * @param data Data to send
 * @param len Data length
 * @return Number of bytes sent, or -1 on error
 */
ssize_t tcp_connection_send(tcp_connection_t *conn, const uint8_t *data, size_t len);

/**
 * Receive data on TCP connection (blocking)
 * @param conn Connection handle
 * @param buffer Buffer to receive into
 * @param buffer_size Buffer size
 * @return Number of bytes received, 0 on EOF, -1 on error
 */
ssize_t tcp_connection_recv(tcp_connection_t *conn, uint8_t *buffer, size_t buffer_size);

/**
 * Close TCP connection
 * @param conn Connection handle
 */
void tcp_connection_close(tcp_connection_t *conn);

/**
 * Destroy TCP transport (server mode)
 * @param transport Transport handle
 */
void tcp_transport_destroy(tcp_transport_t *transport);

void tcp_transport_shutdown(tcp_transport_t *transport);

#endif // ROOLE_TCP_TRANSPORT_H
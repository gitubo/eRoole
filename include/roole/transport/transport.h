// include/roole/transport/transport.h
// Generic transport interface (future: support QUIC, etc.)

#ifndef ROOLE_TRANSPORT_H
#define ROOLE_TRANSPORT_H

#include "roole/core/common.h"
#include <stdint.h>
#include <stddef.h>

// Transport types
typedef enum {
    TRANSPORT_TYPE_UDP = 1,
    TRANSPORT_TYPE_TCP = 2
} transport_type_t;

// Generic transport handle (opaque)
typedef struct transport transport_t;

// Callback invoked when message is received
// Called from transport's receiver thread
typedef void (*transport_recv_callback_fn)(
    const uint8_t *data,
    size_t len,
    const char *src_ip,
    uint16_t src_port,
    void *user_data
);

// Transport operations (vtable pattern)
typedef struct {
    int (*send)(transport_t *transport, const uint8_t *data, size_t len,
                const char *dest_ip, uint16_t dest_port);
    int (*start_receiver)(transport_t *transport, transport_recv_callback_fn callback,
                         void *user_data);
    void (*stop_receiver)(transport_t *transport);
    void (*destroy)(transport_t *transport);
} transport_ops_t;

// Base transport structure (inherited by UDP/TCP implementations)
struct transport {
    transport_type_t type;
    const transport_ops_t *ops;
    char bind_addr[16];
    uint16_t port;
    int socket_fd;
    volatile int shutdown_flag;
};

#endif // ROOLE_TRANSPORT_H
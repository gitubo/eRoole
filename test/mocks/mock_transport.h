// include/roole/test/mock_transport.h
// Mock transport for testing (no real sockets)

#ifndef ROOLE_TEST_MOCK_TRANSPORT_H
#define ROOLE_TEST_MOCK_TRANSPORT_H

#include "roole/transport/transport.h"
#include <stdint.h>

typedef struct mock_transport mock_transport_t;

/**
 * Create mock transport
 * No actual sockets created
 * @return Mock transport handle
 */
mock_transport_t* mock_transport_create(void);

/**
 * Simulate receiving a message
 * Invokes receiver callback as if message arrived via network
 * @param transport Mock transport
 * @param data Message data
 * @param len Message length
 * @param src_ip Source IP (simulated)
 * @param src_port Source port (simulated)
 */
void mock_transport_simulate_receive(mock_transport_t *transport,
                                     const uint8_t *data, size_t len,
                                     const char *src_ip, uint16_t src_port);

/**
 * Get sent messages
 * Returns messages sent via mock_transport_send()
 * @param transport Mock transport
 * @param out_messages Output array of message pointers
 * @param max_count Maximum messages to return
 * @return Number of messages returned
 */
typedef struct {
    uint8_t *data;
    size_t len;
    char dest_ip[16];
    uint16_t dest_port;
} mock_sent_message_t;

size_t mock_transport_get_sent_messages(mock_transport_t *transport,
                                        mock_sent_message_t *out_messages,
                                        size_t max_count);

/**
 * Clear sent messages
 * @param transport Mock transport
 */
void mock_transport_clear_sent_messages(mock_transport_t *transport);

/**
 * Destroy mock transport
 * @param transport Mock transport
 */
void mock_transport_destroy(mock_transport_t *transport);

#endif // ROOLE_TEST_MOCK_TRANSPORT_H
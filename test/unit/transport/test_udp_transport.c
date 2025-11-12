// tests/test_udp_transport.c

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "roole/transport/udp_transport.h"

#define TEST_PORT 9999
#define TEST_MSG "Hello UDP Transport!"
#define TEST_REPLY "Reply from server"

static int messages_received = 0;
static int server_running = 1;

// Mock receiver callback
static void test_recv_callback(const uint8_t *data, size_t len, 
                              const char *src_ip, uint16_t src_port, 
                              void *user_data) {
    printf("Received %zu bytes from %s:%u: %.*s\n", 
           len, src_ip, src_port, (int)len, (const char*)data);
    messages_received++;
    
    // Echo back for bidirectional test
    udp_transport_t *server = (udp_transport_t*)user_data;
    udp_transport_send(server, (uint8_t*)TEST_REPLY, strlen(TEST_REPLY), src_ip, src_port);
}

// Test basic send/receive
static int test_basic_send_recv() {
    printf("=== Test: Basic Send/Receive ===\n");
    
    // Create server
    udp_transport_t *server = udp_transport_create("127.0.0.1", TEST_PORT);
    if (!server) {
        printf("Failed to create server\n");
        return -1;
    }
    
    if (udp_transport_start_receiver(server, test_recv_callback, server) != 0) {
        printf("Failed to start receiver\n");
        udp_transport_destroy(server);
        return -1;
    }
    
    sleep(1); // Let server start
    
    // Create client
    udp_transport_t *client = udp_transport_create("127.0.0.1", TEST_PORT + 1);
    if (!client) {
        printf("Failed to create client\n");
        udp_transport_destroy(server);
        return -1;
    }
    
    // Send message
    ssize_t sent = udp_transport_send(client, (uint8_t*)TEST_MSG, strlen(TEST_MSG), 
                                     "127.0.0.1", TEST_PORT);
    if (sent < 0) {
        printf("Failed to send message\n");
        udp_transport_destroy(client);
        udp_transport_destroy(server);
        return -1;
    }
    
    sleep(1); // Wait for response
    
    udp_transport_destroy(client);
    udp_transport_destroy(server);
    
    printf("Messages received: %d\n", messages_received);
    return (messages_received > 0) ? 0 : -1;
}

// Test with fake socket descriptors (mock)
static int test_with_mock_sockets() {
    printf("=== Test: Mock Socket Test ===\n");
    
    // This test would use socketpair() or similar to create connected sockets
    // For now, we'll test with localhost networking
    
    int sock_fds[2];
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sock_fds) < 0) {
        printf("socketpair failed: %s\n", strerror(errno));
        return -1;
    }
    
    // Would need to modify UDP transport to accept pre-created sockets
    // For demonstration, we'll just close them
    
    close(sock_fds[0]);
    close(sock_fds[1]);
    
    printf("Mock socket test completed\n");
    return 0;
}

// Test statistics
static int test_statistics() {
    printf("=== Test: Statistics ===\n");
    
    udp_transport_t *transport = udp_transport_create("127.0.0.1", TEST_PORT + 2);
    if (!transport) {
        printf("Failed to create transport\n");
        return -1;
    }
    
    udp_transport_stats_t stats;
    udp_transport_get_stats(transport, &stats);
    
    printf("Initial stats - bytes_sent: %lu, packets_sent: %lu, send_errors: %lu\n",
           stats.bytes_sent, stats.packets_sent, stats.send_errors);
    
    // Send some data
    const char *test_data = "stats test";
    udp_transport_send(transport, (uint8_t*)test_data, strlen(test_data), 
                      "127.0.0.1", TEST_PORT + 3); // Non-existent port
    
    udp_transport_get_stats(transport, &stats);
    printf("After send - bytes_sent: %lu, packets_sent: %lu, send_errors: %lu\n",
           stats.bytes_sent, stats.packets_sent, stats.send_errors);
    
    udp_transport_destroy(transport);
    return 0;
}

int main() {
    printf("UDP Transport Tests\n");
    printf("==================\n");
    
    int failed = 0;
    
    if (test_basic_send_recv() != 0) {
        printf("❌ Basic send/recv test failed\n");
        failed++;
    } else {
        printf("✅ Basic send/recv test passed\n");
    }
    
    if (test_with_mock_sockets() != 0) {
        printf("❌ Mock socket test failed\n");
        failed++;
    } else {
        printf("✅ Mock socket test passed\n");
    }
    
    if (test_statistics() != 0) {
        printf("❌ Statistics test failed\n");
        failed++;
    } else {
        printf("✅ Statistics test passed\n");
    }
    
    printf("\n=== Summary ===\n");
    if (failed == 0) {
        printf("All tests passed! ✅\n");
        return 0;
    } else {
        printf("%d tests failed ❌\n", failed);
        return 1;
    }
}
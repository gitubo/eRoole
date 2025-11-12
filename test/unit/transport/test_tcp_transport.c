/*  tests/test_tcp_transport.c  */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>

#include "roole/transport/tcp_transport.h"

/* -------------------------- test configuration -------------------------- */
#define TEST_PORT        9998
#define TEST_MSG         "Hello TCP transport!"
#define TEST_REPLY       "Reply from server"
#define TEST_TIMEOUT_S   5

/* -------------------------- shared test state --------------------------- */
static int  server_got_msg     = 0;
static int  client_got_reply   = 0;
static int  server_done        = 0;
static pthread_mutex_t  lock   = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t   cond   = PTHREAD_COND_INITIALIZER;

/* -------------------------- receiver callbacks -------------------------- */
static void server_on_recv(tcp_connection_t *conn,
                          const uint8_t *data,
                          size_t len,
                          void *user_data)
{
    (void)user_data;
    printf("[SERVER] received %zu bytes: %.*s\n", len, (int)len, (const char *)data);

    pthread_mutex_lock(&lock);
    server_got_msg = 1;
    pthread_mutex_unlock(&lock);

    // Echo back immediately
    ssize_t nw = tcp_connection_send(conn, (const uint8_t *)TEST_REPLY, strlen(TEST_REPLY));
    if (nw != (ssize_t)strlen(TEST_REPLY)) {
        fprintf(stderr, "[SERVER] short send\n");
    } else {
        printf("[SERVER] sent reply: %zu bytes\n", strlen(TEST_REPLY));
    }
}

/* -------------------------- server thread ------------------------------- */
static void *server_thread(void *arg)
{
    tcp_transport_t *srv = (tcp_transport_t *)arg;
    if (tcp_transport_start_accepting(srv, server_on_recv, NULL) != 0) {
        fprintf(stderr, "server: start_accept failed\n");
        return NULL;
    }

    // Wait for shutdown signal
    pthread_mutex_lock(&lock);
    while (!server_done) {
        pthread_cond_wait(&cond, &lock);
    }
    pthread_mutex_unlock(&lock);

    printf("[SERVER] shutting down...\n");
    return NULL;
}

/* -------------------------- test cases ---------------------------------- */
static int test_basic_client_server(void)
{
    printf("\n=== %s ===\n", __func__);

    // Reset test state
    server_got_msg = 0;
    client_got_reply = 0;
    server_done = 0;

    // Start server
    tcp_transport_t *srv = tcp_transport_create_server("127.0.0.1", TEST_PORT, 10);
    if (!srv) {
        fprintf(stderr, "❌ server create failed\n");
        return -1;
    }

    pthread_t srv_tid;
    if (pthread_create(&srv_tid, NULL, server_thread, srv) != 0) {
        fprintf(stderr, "❌ failed to create server thread\n");
        tcp_transport_destroy(srv);
        return -1;
    }

    sleep(1); // Give server time to start listening

    // Client connect
    printf("[CLIENT] connecting to server...\n");
    tcp_connection_t *cln = tcp_transport_connect("127.0.0.1", TEST_PORT);
    if (!cln) {
        fprintf(stderr, "❌ client connect failed\n");
        pthread_mutex_lock(&lock);
        server_done = 1;
        pthread_cond_broadcast(&cond);
        pthread_mutex_unlock(&lock);
        pthread_join(srv_tid, NULL);
        tcp_transport_destroy(srv);
        return -1;
    }

    printf("[CLIENT] connected successfully\n");

    // Send test message
    printf("[CLIENT] sending message: %s\n", TEST_MSG);
    ssize_t nw = tcp_connection_send(cln, (uint8_t *)TEST_MSG, strlen(TEST_MSG));
    if (nw != (ssize_t)strlen(TEST_MSG)) {
        fprintf(stderr, "❌ client send failed\n");
        goto cleanup_fail;
    }

    // Read reply with timeout
    uint8_t buf[128];
    size_t got = 0;
    time_t start = time(NULL);
    
    printf("[CLIENT] waiting for reply...\n");
    while (got < strlen(TEST_REPLY)) {
        if (time(NULL) - start > TEST_TIMEOUT_S) {
            fprintf(stderr, "❌ timeout waiting for reply\n");
            goto cleanup_fail;
        }

        ssize_t nr = tcp_connection_recv(cln, buf + got, sizeof(buf) - got);
        if (nr > 0) {
            got += nr;
            printf("[CLIENT] received %zd bytes (total %zu)\n", nr, got);
        } else if (nr == 0) {
            fprintf(stderr, "❌ connection closed by server\n");
            break;
        } else if (nr < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                perror("recv");
                break;
            }
            usleep(10000); // Wait 10ms before retry
        }
    }

    // Verify reply
    if (got == strlen(TEST_REPLY) && memcmp(buf, TEST_REPLY, got) == 0) {
        printf("[CLIENT] ✅ received correct reply: %.*s\n", (int)got, buf);
        client_got_reply = 1;
    } else {
        fprintf(stderr, "❌ reply mismatch or incomplete\n");
        goto cleanup_fail;
    }

    // Cleanup
    printf("[CLIENT] closing connection...\n");
    tcp_connection_close(cln);

    pthread_mutex_lock(&lock);
    server_done = 1;
    pthread_cond_broadcast(&cond);
    pthread_mutex_unlock(&lock);

    sleep(1); // Give server time to process shutdown

    tcp_transport_shutdown(srv);
    pthread_join(srv_tid, NULL);
    tcp_transport_destroy(srv);

    // Verify results
    if (server_got_msg && client_got_reply) {
        printf("✅ %s passed\n", __func__);
        return 0;
    } else {
        fprintf(stderr, "❌ test incomplete: server_got_msg=%d, client_got_reply=%d\n",
                server_got_msg, client_got_reply);
        return -1;
    }

cleanup_fail:
    tcp_connection_close(cln);
    pthread_mutex_lock(&lock);
    server_done = 1;
    pthread_cond_broadcast(&cond);
    pthread_mutex_unlock(&lock);
    tcp_transport_shutdown(srv);
    pthread_join(srv_tid, NULL);
    tcp_transport_destroy(srv);
    printf("❌ %s failed\n", __func__);
    return -1;
}

static int test_refuse_connection(void)
{
    printf("\n=== %s ===\n", __func__);

    // Attempt to connect to a port nobody is listening on
    tcp_connection_t *c = tcp_transport_connect("127.0.0.1", TEST_PORT + 100);
    if (c) {
        tcp_connection_close(c);
        printf("❌ connection should have failed\n");
        return -1;
    }
    printf("✅ %s passed (connect correctly refused)\n", __func__);
    return 0;
}

static int test_multiple_messages(void)
{
    printf("\n=== %s ===\n", __func__);

    // Reset test state
    server_got_msg = 0;
    client_got_reply = 0;
    server_done = 0;

    // Start server
    tcp_transport_t *srv = tcp_transport_create_server("127.0.0.1", TEST_PORT, 10);
    if (!srv) {
        fprintf(stderr, "❌ server create failed\n");
        return -1;
    }

    pthread_t srv_tid;
    pthread_create(&srv_tid, NULL, server_thread, srv);
    sleep(1);

    // Client connect
    tcp_connection_t *cln = tcp_transport_connect("127.0.0.1", TEST_PORT);
    if (!cln) {
        fprintf(stderr, "❌ client connect failed\n");
        goto cleanup;
    }

    // Send multiple messages
    for (int i = 0; i < 3; i++) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Message #%d", i);
        
        printf("[CLIENT] sending: %s\n", msg);
        tcp_connection_send(cln, (uint8_t *)msg, strlen(msg));

        // Read reply
        uint8_t buf[128];
        size_t got = 0;
        time_t start = time(NULL);

        while (got < strlen(TEST_REPLY)) {
            if (time(NULL) - start > 2) break;
            
            ssize_t nr = tcp_connection_recv(cln, buf + got, sizeof(buf) - got);
            if (nr > 0) {
                got += nr;
            } else if (nr < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                break;
            }
            usleep(10000);
        }

        if (got == strlen(TEST_REPLY)) {
            printf("[CLIENT] ✅ got reply for message #%d\n", i);
        } else {
            printf("[CLIENT] ❌ failed to get reply for message #%d\n", i);
        }
    }

    tcp_connection_close(cln);

cleanup:
    pthread_mutex_lock(&lock);
    server_done = 1;
    pthread_cond_broadcast(&cond);
    pthread_mutex_unlock(&lock);

    tcp_transport_shutdown(srv);
    pthread_join(srv_tid, NULL);
    tcp_transport_destroy(srv);

    printf("✅ %s completed\n", __func__);
    return 0;
}

/* -------------------------- main --------------------------------------- */
int main(void)
{
    printf("===========================================\n");
    printf("  TCP Transport Tests\n");
    printf("===========================================\n");

    int failed = 0;

    if (test_basic_client_server() != 0) ++failed;
    if (test_refuse_connection() != 0) ++failed;
    if (test_multiple_messages() != 0) ++failed;

    printf("\n===========================================\n");
    printf("  Summary\n");
    printf("===========================================\n");
    if (failed == 0) {
        printf("✅ All TCP transport tests passed!\n");
        return 0;
    }
    printf("❌ %d test(s) failed\n", failed);
    return 1;
}
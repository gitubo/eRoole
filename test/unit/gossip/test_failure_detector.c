// test/unit/gossip/test_failure_detector.c
// Tests for failure detector edge cases

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include "roole/gossip/gossip_protocol.h"
#include "roole/cluster/cluster_view.h"

typedef struct {
    cluster_view_t cluster_view;
    gossip_protocol_t *protocol;
    
    int alive_count;
    int suspect_count;
    int dead_count;
    int send_count;
} test_fixture_t;

static void mock_on_member_alive(node_id_t node_id,
                                 const gossip_member_update_t *update,
                                 void *ctx)
{
    (void)update;
    test_fixture_t *fixture = (test_fixture_t*)ctx;
    fixture->alive_count++;
    printf("[TEST] Node %u is ALIVE\n", node_id);
}

static void mock_on_member_suspect(node_id_t node_id,
                                   uint64_t incarnation,
                                   void *ctx)
{
    test_fixture_t *fixture = (test_fixture_t*)ctx;
    fixture->suspect_count++;
    printf("[TEST] Node %u is SUSPECT (inc=%lu)\n", node_id, incarnation);
}

static void mock_on_member_dead(node_id_t node_id, void *ctx)
{
    test_fixture_t *fixture = (test_fixture_t*)ctx;
    fixture->dead_count++;
    printf("[TEST] Node %u is DEAD\n", node_id);
}

static void mock_on_send_message(const gossip_message_t *msg,
                                 const char *dest_ip,
                                 uint16_t dest_port,
                                 void *ctx)
{
    (void)msg;
    (void)dest_ip;
    (void)dest_port;
    test_fixture_t *fixture = (test_fixture_t*)ctx;
    fixture->send_count++;
}

static void setup_fixture(test_fixture_t *fixture)
{
    memset(fixture, 0, sizeof(test_fixture_t));
    
    cluster_view_init(&fixture->cluster_view, 100);
    
    gossip_config_t config = gossip_default_config();
    config.ack_timeout_ms = 100;
    config.dead_timeout_ms = 500;
    
    gossip_protocol_callbacks_t callbacks = {
        .on_member_alive = mock_on_member_alive,
        .on_member_suspect = mock_on_member_suspect,
        .on_member_dead = mock_on_member_dead,
        .on_send_message = mock_on_send_message
    };
    
    fixture->protocol = gossip_protocol_create(
        1, NODE_TYPE_ROUTER, "127.0.0.1", 8000, 9000,
        &config, &fixture->cluster_view, &callbacks, fixture
    );
    
    assert(fixture->protocol != NULL);
}

static void teardown_fixture(test_fixture_t *fixture)
{
    if (fixture->protocol) {
        gossip_protocol_destroy(fixture->protocol);
    }
    cluster_view_destroy(&fixture->cluster_view);
}

// ============================================================================
// TEST CASES
// ============================================================================

static int test_swim_round_with_no_peers()
{
    printf("\n=== Test: SWIM Round with No Peers ===\n");
    
    test_fixture_t fixture;
    setup_fixture(&fixture);
    
    // Run SWIM round with empty cluster
    gossip_protocol_run_swim_round(fixture.protocol);
    
    // Should not crash or send anything
    assert(fixture.send_count == 0);
    
    teardown_fixture(&fixture);
    printf("✅ Test passed\n");
    return 0;
}

static int test_swim_round_with_only_dead_peers()
{
    printf("\n=== Test: SWIM Round with Only DEAD Peers ===\n");
    
    test_fixture_t fixture;
    setup_fixture(&fixture);
    
    // Add DEAD peers
    for (int i = 2; i <= 5; i++) {
        cluster_member_t peer = {
            .node_id = i,
            .node_type = NODE_TYPE_ROUTER,
            .status = NODE_STATUS_DEAD,
            .incarnation = 0,
            .gossip_port = 8000 + i,
            .data_port = 9000 + i
        };
        snprintf(peer.ip_address, MAX_IP_LEN, "127.0.0.%d", i);
        cluster_view_add(&fixture.cluster_view, &peer);
    }
    
    // Run SWIM round
    gossip_protocol_run_swim_round(fixture.protocol);
    
    // Should not send PING (no alive peers)
    assert(fixture.send_count == 0);
    
    teardown_fixture(&fixture);
    printf("✅ Test passed\n");
    return 0;
}

static int test_multiple_pending_acks()
{
    printf("\n=== Test: Multiple Pending ACKs ===\n");
    
    test_fixture_t fixture;
    setup_fixture(&fixture);
    
    // Add multiple peers
    for (int i = 2; i <= 10; i++) {
        cluster_member_t peer = {
            .node_id = i,
            .node_type = NODE_TYPE_ROUTER,
            .status = NODE_STATUS_ALIVE,
            .incarnation = 0,
            .gossip_port = 8000 + i,
            .data_port = 9000 + i
        };
        snprintf(peer.ip_address, MAX_IP_LEN, "127.0.0.%d", i);
        cluster_view_add(&fixture.cluster_view, &peer);
    }
    
    // Run multiple SWIM rounds to generate pending ACKs
    for (int round = 0; round < 5; round++) {
        gossip_protocol_run_swim_round(fixture.protocol);
        usleep(50000); // 50ms
    }
    
    printf("Sent %d PINGs\n", fixture.send_count);
    assert(fixture.send_count == 5);
    
    // Wait for timeouts
    usleep(150000); // 150ms
    
    gossip_protocol_check_timeouts(fixture.protocol);
    
    // Some nodes should be marked SUSPECT
    printf("Suspect count: %d\n", fixture.suspect_count);
    assert(fixture.suspect_count > 0);
    
    teardown_fixture(&fixture);
    printf("✅ Test passed\n");
    return 0;
}

static int test_suspect_then_alive_then_suspect_again()
{
    printf("\n=== Test: SUSPECT → ALIVE → SUSPECT (Flapping) ===\n");
    
    test_fixture_t fixture;
    setup_fixture(&fixture);
    
    // Add node 2 as ALIVE
    cluster_member_t peer = {
        .node_id = 2,
        .node_type = NODE_TYPE_ROUTER,
        .status = NODE_STATUS_ALIVE,
        .incarnation = 0,
        .gossip_port = 8001,
        .data_port = 9001
    };
    safe_strncpy(peer.ip_address, "127.0.0.2", MAX_IP_LEN);
    cluster_view_add(&fixture.cluster_view, &peer);
    
    // Mark as SUSPECT (incarnation 0)
    cluster_view_update_status(&fixture.cluster_view, 2, NODE_STATUS_SUSPECT, 0);
    
    // Node 2 refutes with ALIVE (incarnation 1)
    gossip_message_t alive1 = {
        .version = 1,
        .msg_type = GOSSIP_MSG_ALIVE,
        .sender_id = 2,
        .num_updates = 1
    };
    alive1.updates[0].node_id = 2;
    alive1.updates[0].status = NODE_STATUS_ALIVE;
    alive1.updates[0].incarnation = 1;
    
    gossip_protocol_handle_message(fixture.protocol, &alive1, "127.0.0.2", 8001);
    
    cluster_member_t *m2 = cluster_view_get(&fixture.cluster_view, 2);
    assert(m2 != NULL && m2->status == NODE_STATUS_ALIVE);
    assert(m2->incarnation == 1);
    cluster_view_release(&fixture.cluster_view);
    
    // Mark as SUSPECT again (incarnation 1)
    cluster_view_update_status(&fixture.cluster_view, 2, NODE_STATUS_SUSPECT, 1);
    
    // Node 2 refutes again with ALIVE (incarnation 2)
    gossip_message_t alive2 = {
        .version = 1,
        .msg_type = GOSSIP_MSG_ALIVE,
        .sender_id = 2,
        .num_updates = 1
    };
    alive2.updates[0].node_id = 2;
    alive2.updates[0].status = NODE_STATUS_ALIVE;
    alive2.updates[0].incarnation = 2;
    
    gossip_protocol_handle_message(fixture.protocol, &alive2, "127.0.0.2", 8001);
    
    m2 = cluster_view_get(&fixture.cluster_view, 2);
    assert(m2 != NULL && m2->status == NODE_STATUS_ALIVE);
    assert(m2->incarnation == 2);
    cluster_view_release(&fixture.cluster_view);
    
    teardown_fixture(&fixture);
    printf("✅ Test passed\n");
    return 0;
}

static int test_dead_node_timeout_cleanup()
{
    printf("\n=== Test: DEAD Node Timeout Cleanup ===\n");
    
    test_fixture_t fixture;
    setup_fixture(&fixture);
    
    // Add multiple SUSPECT nodes with different timestamps
    for (int i = 2; i <= 5; i++) {
        cluster_member_t peer = {
            .node_id = i,
            .node_type = NODE_TYPE_ROUTER,
            .status = NODE_STATUS_SUSPECT,
            .incarnation = 0,
            .gossip_port = 8000 + i,
            .data_port = 9000 + i,
            .last_seen_ms = time_now_ms() - (600 * i)  // Different ages
        };
        snprintf(peer.ip_address, MAX_IP_LEN, "127.0.0.%d", i);
        cluster_view_add(&fixture.cluster_view, &peer);
    }
    
    // Check timeouts
    gossip_protocol_check_timeouts(fixture.protocol);
    
    // Nodes with old timestamps should be marked DEAD
    printf("Dead count: %d\n", fixture.dead_count);
    assert(fixture.dead_count >= 2);  // At least nodes 2 and 3 (oldest)
    
    teardown_fixture(&fixture);
    printf("✅ Test passed\n");
    return 0;
}

// ============================================================================
// TEST RUNNER
// ============================================================================

int main(void)
{
    printf("===========================================\n");
    printf("  Failure Detector Edge Case Tests\n");
    printf("===========================================\n");
    
    int failed = 0;
    
    if (test_swim_round_with_no_peers() != 0) failed++;
    if (test_swim_round_with_only_dead_peers() != 0) failed++;
    if (test_multiple_pending_acks() != 0) failed++;
    if (test_suspect_then_alive_then_suspect_again() != 0) failed++;
    if (test_dead_node_timeout_cleanup() != 0) failed++;
    
    printf("\n===========================================\n");
    printf("  Summary\n");
    printf("===========================================\n");
    if (failed == 0) {
        printf("✅ All failure detector tests passed!\n");
        return 0;
    }
    printf("❌ %d test(s) failed\n", failed);
    return 1;
}
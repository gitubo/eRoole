// test/unit/gossip/test_swim_state.c
// Unit tests for SWIM state machine WITHOUT network I/O

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "roole/gossip/gossip_protocol.h"
#include "roole/cluster/cluster_view.h"

// Test fixture
typedef struct {
    cluster_view_t cluster_view;
    gossip_protocol_t *protocol;
    
    // Track callback invocations
    int alive_count;
    int suspect_count;
    int dead_count;
    int send_count;
    
    node_id_t last_alive_node;
    node_id_t last_suspect_node;
    node_id_t last_dead_node;
    
    gossip_message_t last_sent_msg;
    char last_dest_ip[MAX_IP_LEN];
    uint16_t last_dest_port;
} test_fixture_t;

// Mock callbacks
static void mock_on_member_alive(node_id_t node_id,
                                 const gossip_member_update_t *update,
                                 void *ctx)
{
    test_fixture_t *fixture = (test_fixture_t*)ctx;
    fixture->alive_count++;
    fixture->last_alive_node = node_id;
    printf("[TEST] Callback: Node %u is ALIVE\n", node_id);
}

static void mock_on_member_suspect(node_id_t node_id,
                                   uint64_t incarnation,
                                   void *ctx)
{
    test_fixture_t *fixture = (test_fixture_t*)ctx;
    fixture->suspect_count++;
    fixture->last_suspect_node = node_id;
    printf("[TEST] Callback: Node %u is SUSPECT (inc=%lu)\n", node_id, incarnation);
}

static void mock_on_member_dead(node_id_t node_id, void *ctx)
{
    test_fixture_t *fixture = (test_fixture_t*)ctx;
    fixture->dead_count++;
    fixture->last_dead_node = node_id;
    printf("[TEST] Callback: Node %u is DEAD\n", node_id);
}

static void mock_on_send_message(const gossip_message_t *msg,
                                 const char *dest_ip,
                                 uint16_t dest_port,
                                 void *ctx)
{
    test_fixture_t *fixture = (test_fixture_t*)ctx;
    fixture->send_count++;
    fixture->last_sent_msg = *msg;
    
    if (dest_ip) {
        safe_strncpy(fixture->last_dest_ip, dest_ip, MAX_IP_LEN);
        fixture->last_dest_port = dest_port;
        printf("[TEST] Send: type=%u to %s:%u updates=%u\n",
               msg->msg_type, dest_ip, dest_port, msg->num_updates);
    } else {
        fixture->last_dest_ip[0] = '\0';
        fixture->last_dest_port = 0;
        printf("[TEST] Send: type=%u (broadcast) updates=%u\n",
               msg->msg_type, msg->num_updates);
    }
}

// Setup and teardown
static void setup_fixture(test_fixture_t *fixture)
{
    memset(fixture, 0, sizeof(test_fixture_t));
    
    cluster_view_init(&fixture->cluster_view, 100);
    
    gossip_config_t config = gossip_default_config();
    config.ack_timeout_ms = 500;
    config.dead_timeout_ms = 2000;
    
    gossip_protocol_callbacks_t callbacks = {
        .on_member_alive = mock_on_member_alive,
        .on_member_suspect = mock_on_member_suspect,
        .on_member_dead = mock_on_member_dead,
        .on_send_message = mock_on_send_message
    };
    
    fixture->protocol = gossip_protocol_create(
        1,                      // my_id
        NODE_TYPE_ROUTER,       // my_type
        "127.0.0.1",           // my_ip
        8000,                  // gossip_port
        9000,                  // data_port
        &config,
        &fixture->cluster_view,
        &callbacks,
        fixture
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

static int test_handle_ping_new_member()
{
    printf("\n=== Test: Handle PING - New Member Discovery ===\n");
    
    test_fixture_t fixture;
    setup_fixture(&fixture);
    
    // Create PING message from unknown node
    gossip_message_t ping = {
        .version = 1,
        .msg_type = GOSSIP_MSG_PING,
        .sender_id = 2,
        .sequence_num = 1,
        .num_updates = 1
    };
    
    ping.updates[0].node_id = 2;
    ping.updates[0].node_type = NODE_TYPE_ROUTER;
    ping.updates[0].status = NODE_STATUS_ALIVE;
    ping.updates[0].incarnation = 0;
    ping.updates[0].gossip_port = 8001;
    ping.updates[0].data_port = 9001;
    safe_strncpy(ping.updates[0].ip_address, "127.0.0.2", MAX_IP_LEN);
    
    // Handle PING
    gossip_protocol_handle_message(fixture.protocol, &ping, "127.0.0.2", 8001);
    
    // Verify new member was discovered
    cluster_member_t *member = cluster_view_get(&fixture.cluster_view, 2);
    assert(member != NULL);
    assert(member->status == NODE_STATUS_ALIVE);
    cluster_view_release(&fixture.cluster_view);
    
    // Verify callback was invoked
    assert(fixture.alive_count == 1);
    assert(fixture.last_alive_node == 2);
    
    // Verify ACK was sent
    assert(fixture.send_count == 1);
    assert(fixture.last_sent_msg.msg_type == GOSSIP_MSG_ACK);
    assert(strcmp(fixture.last_dest_ip, "127.0.0.2") == 0);
    assert(fixture.last_dest_port == 8001);
    
    teardown_fixture(&fixture);
    printf("✅ Test passed\n");
    return 0;
}

static int test_handle_ack_clears_pending()
{
    printf("\n=== Test: Handle ACK - Clears Pending ACK ===\n");
    
    test_fixture_t fixture;
    setup_fixture(&fixture);
    
    // Add peer to cluster
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
    
    // Run SWIM round (should send PING)
    gossip_protocol_run_swim_round(fixture.protocol);
    
    assert(fixture.send_count == 1);
    assert(fixture.last_sent_msg.msg_type == GOSSIP_MSG_PING);
    
    // Create ACK response
    gossip_message_t ack = {
        .version = 1,
        .msg_type = GOSSIP_MSG_ACK,
        .sender_id = 2,
        .sequence_num = 1,
        .num_updates = 0
    };
    
    // Handle ACK
    gossip_protocol_handle_message(fixture.protocol, &ack, "127.0.0.2", 8001);
    
    // Verify pending ACK was cleared (no timeout should occur)
    usleep(600000); // Wait longer than ack_timeout_ms (500ms)
    gossip_protocol_check_timeouts(fixture.protocol);
    
    // Node should still be ALIVE (no SUSPECT callback)
    assert(fixture.suspect_count == 0);
    
    cluster_member_t *member = cluster_view_get(&fixture.cluster_view, 2);
    assert(member != NULL);
    assert(member->status == NODE_STATUS_ALIVE);
    cluster_view_release(&fixture.cluster_view);
    
    teardown_fixture(&fixture);
    printf("✅ Test passed\n");
    return 0;
}

static int test_ack_timeout_marks_suspect()
{
    printf("\n=== Test: ACK Timeout - Marks Node as SUSPECT ===\n");
    
    test_fixture_t fixture;
    setup_fixture(&fixture);
    
    // Add peer to cluster
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
    
    // Run SWIM round (should send PING)
    gossip_protocol_run_swim_round(fixture.protocol);
    assert(fixture.send_count == 1);
    
    // Wait for ACK timeout
    usleep(600000); // Wait 600ms (> 500ms timeout)
    
    // Check timeouts
    gossip_protocol_check_timeouts(fixture.protocol);
    
    // Verify node is now SUSPECT
    cluster_member_t *member = cluster_view_get(&fixture.cluster_view, 2);
    assert(member != NULL);
    assert(member->status == NODE_STATUS_SUSPECT);
    cluster_view_release(&fixture.cluster_view);
    
    // Verify callback was invoked
    assert(fixture.suspect_count == 1);
    assert(fixture.last_suspect_node == 2);
    
    teardown_fixture(&fixture);
    printf("✅ Test passed\n");
    return 0;
}

static int test_suspect_timeout_marks_dead()
{
    printf("\n=== Test: SUSPECT Timeout - Marks Node as DEAD ===\n");
    
    test_fixture_t fixture;
    setup_fixture(&fixture);
    
    // Add SUSPECT peer to cluster
    cluster_member_t peer = {
        .node_id = 2,
        .node_type = NODE_TYPE_ROUTER,
        .status = NODE_STATUS_SUSPECT,
        .incarnation = 0,
        .gossip_port = 8001,
        .data_port = 9001,
        .last_seen_ms = time_now_ms() - 2100 // Older than dead_timeout (2000ms)
    };
    safe_strncpy(peer.ip_address, "127.0.0.2", MAX_IP_LEN);
    cluster_view_add(&fixture.cluster_view, &peer);
    
    // Check timeouts
    gossip_protocol_check_timeouts(fixture.protocol);
    
    // Verify node is now DEAD
    cluster_member_t *member = cluster_view_get(&fixture.cluster_view, 2);
    assert(member != NULL);
    assert(member->status == NODE_STATUS_DEAD);
    cluster_view_release(&fixture.cluster_view);
    
    // Verify callback was invoked
    assert(fixture.dead_count == 1);
    assert(fixture.last_dead_node == 2);
    
    teardown_fixture(&fixture);
    printf("✅ Test passed\n");
    return 0;
}

static int test_refute_suspicion()
{
    printf("\n=== Test: Refute Suspicion - Increase Incarnation ===\n");
    
    test_fixture_t fixture;
    setup_fixture(&fixture);
    
    // Get initial incarnation
    gossip_protocol_stats_t stats;
    gossip_protocol_get_stats(fixture.protocol, &stats);
    
    // Create SUSPECT message targeting us
    gossip_message_t suspect = {
        .version = 1,
        .msg_type = GOSSIP_MSG_SUSPECT,
        .sender_id = 2,
        .sequence_num = 1,
        .num_updates = 1
    };
    
    suspect.updates[0].node_id = 1; // Our node
    suspect.updates[0].status = NODE_STATUS_SUSPECT;
    suspect.updates[0].incarnation = 0;
    
    // Handle SUSPECT
    gossip_protocol_handle_message(fixture.protocol, &suspect, "127.0.0.2", 8001);
    
    // Verify we sent ALIVE message
    assert(fixture.send_count == 1);
    assert(fixture.last_sent_msg.msg_type == GOSSIP_MSG_ALIVE);
    assert(fixture.last_sent_msg.num_updates == 1);
    assert(fixture.last_sent_msg.updates[0].node_id == 1);
    assert(fixture.last_sent_msg.updates[0].status == NODE_STATUS_ALIVE);
    assert(fixture.last_sent_msg.updates[0].incarnation > 0); // Incarnation increased
    
    teardown_fixture(&fixture);
    printf("✅ Test passed\n");
    return 0;
}

static int test_handle_rejoin()
{
    printf("\n=== Test: Handle Rejoin - DEAD -> ALIVE with Higher Incarnation ===\n");
    
    test_fixture_t fixture;
    setup_fixture(&fixture);
    
    // Add DEAD peer to cluster
    cluster_member_t peer = {
        .node_id = 2,
        .node_type = NODE_TYPE_ROUTER,
        .status = NODE_STATUS_DEAD,
        .incarnation = 5,
        .gossip_port = 8001,
        .data_port = 9001
    };
    safe_strncpy(peer.ip_address, "127.0.0.2", MAX_IP_LEN);
    cluster_view_add(&fixture.cluster_view, &peer);
    
    // Create PING with rejoin (higher incarnation)
    gossip_message_t ping = {
        .version = 1,
        .msg_type = GOSSIP_MSG_PING,
        .sender_id = 3,
        .sequence_num = 1,
        .num_updates = 1
    };
    
    ping.updates[0].node_id = 2;
    ping.updates[0].node_type = NODE_TYPE_ROUTER;
    ping.updates[0].status = NODE_STATUS_ALIVE;
    ping.updates[0].incarnation = 10; // Higher than 5
    ping.updates[0].gossip_port = 8001;
    ping.updates[0].data_port = 9001;
    safe_strncpy(ping.updates[0].ip_address, "127.0.0.2", MAX_IP_LEN);
    
    // Handle PING
    gossip_protocol_handle_message(fixture.protocol, &ping, "127.0.0.3", 8002);
    
    // Verify node is now ALIVE with new incarnation
    cluster_member_t *member = cluster_view_get(&fixture.cluster_view, 2);
    assert(member != NULL);
    assert(member->status == NODE_STATUS_ALIVE);
    assert(member->incarnation == 10);
    cluster_view_release(&fixture.cluster_view);
    
    // Verify callback was invoked
    assert(fixture.alive_count == 1);
    
    teardown_fixture(&fixture);
    printf("✅ Test passed\n");
    return 0;
}

static int test_ignore_stale_updates()
{
    printf("\n=== Test: Ignore Stale Updates - Lower Incarnation ===\n");
    
    test_fixture_t fixture;
    setup_fixture(&fixture);
    
    // Add peer with incarnation 10
    cluster_member_t peer = {
        .node_id = 2,
        .node_type = NODE_TYPE_ROUTER,
        .status = NODE_STATUS_ALIVE,
        .incarnation = 10,
        .gossip_port = 8001,
        .data_port = 9001
    };
    safe_strncpy(peer.ip_address, "127.0.0.2", MAX_IP_LEN);
    cluster_view_add(&fixture.cluster_view, &peer);
    
    // Create PING with stale update (incarnation 5)
    gossip_message_t ping = {
        .version = 1,
        .msg_type = GOSSIP_MSG_PING,
        .sender_id = 3,
        .sequence_num = 1,
        .num_updates = 1
    };
    
    ping.updates[0].node_id = 2;
    ping.updates[0].status = NODE_STATUS_SUSPECT;
    ping.updates[0].incarnation = 5; // Lower than 10
    
    // Handle PING
    gossip_protocol_handle_message(fixture.protocol, &ping, "127.0.0.3", 8002);
    
    // Verify status was NOT changed
    cluster_member_t *member = cluster_view_get(&fixture.cluster_view, 2);
    assert(member != NULL);
    assert(member->status == NODE_STATUS_ALIVE); // Still ALIVE
    assert(member->incarnation == 10); // Still 10
    cluster_view_release(&fixture.cluster_view);
    
    // Verify no SUSPECT callback
    assert(fixture.suspect_count == 0);
    
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
    printf("  SWIM State Machine Unit Tests\n");
    printf("===========================================\n");
    
    int failed = 0;
    
    if (test_handle_ping_new_member() != 0) failed++;
    if (test_handle_ack_clears_pending() != 0) failed++;
    if (test_ack_timeout_marks_suspect() != 0) failed++;
    if (test_suspect_timeout_marks_dead() != 0) failed++;
    if (test_refute_suspicion() != 0) failed++;
    if (test_handle_rejoin() != 0) failed++;
    if (test_ignore_stale_updates() != 0) failed++;
    
    printf("\n===========================================\n");
    printf("  Summary\n");
    printf("===========================================\n");
    if (failed == 0) {
        printf("✅ All SWIM state tests passed!\n");
        return 0;
    }
    printf("❌ %d test(s) failed\n", failed);
    return 1;
}
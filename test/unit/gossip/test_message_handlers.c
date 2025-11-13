// test/unit/gossip/test_message_handlers.c
// Tests for gossip message handlers (anti-entropy, edge cases)

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "roole/gossip/gossip_protocol.h"
#include "roole/cluster/cluster_view.h"

// Test fixture (reuse from test_swim_state.c)
typedef struct {
    cluster_view_t cluster_view;
    gossip_protocol_t *protocol;
    
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
    (void)update;
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
    } else {
        fixture->last_dest_ip[0] = '\0';
        fixture->last_dest_port = 0;
    }
}

static void setup_fixture(test_fixture_t *fixture)
{
    memset(fixture, 0, sizeof(test_fixture_t));
    
    cluster_view_init(&fixture->cluster_view, 100);
    
    gossip_config_t config = gossip_default_config();
    
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

static int test_ping_with_multiple_updates()
{
    printf("\n=== Test: PING with Multiple Updates (Anti-Entropy) ===\n");
    
    test_fixture_t fixture;
    setup_fixture(&fixture);
    
    // Add some nodes to cluster
    for (int i = 2; i <= 4; i++) {
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
    
    // Create PING with 3 piggybacked updates
    gossip_message_t ping = {
        .version = 1,
        .msg_type = GOSSIP_MSG_PING,
        .sender_id = 2,
        .sequence_num = 1,
        .num_updates = 3
    };
    
    // Update 1: Node 3 is SUSPECT
    ping.updates[0].node_id = 3;
    ping.updates[0].node_type = NODE_TYPE_ROUTER;
    ping.updates[0].status = NODE_STATUS_SUSPECT;
    ping.updates[0].incarnation = 1;
    ping.updates[0].gossip_port = 8003;
    ping.updates[0].data_port = 9003;
    safe_strncpy(ping.updates[0].ip_address, "127.0.0.3", MAX_IP_LEN);
    
    // Update 2: Node 4 is DEAD
    ping.updates[1].node_id = 4;
    ping.updates[1].node_type = NODE_TYPE_ROUTER;
    ping.updates[1].status = NODE_STATUS_DEAD;
    ping.updates[1].incarnation = 2;
    ping.updates[1].gossip_port = 8004;
    ping.updates[1].data_port = 9004;
    safe_strncpy(ping.updates[1].ip_address, "127.0.0.4", MAX_IP_LEN);
    
    // Update 3: Node 7 is new (JOIN)
    ping.updates[2].node_id = 7;
    ping.updates[2].node_type = NODE_TYPE_WORKER;
    ping.updates[2].status = NODE_STATUS_ALIVE;
    ping.updates[2].incarnation = 0;
    ping.updates[2].gossip_port = 8007;
    ping.updates[2].data_port = 9007;
    safe_strncpy(ping.updates[2].ip_address, "127.0.0.7", MAX_IP_LEN);
    
    // Handle PING
    gossip_protocol_handle_message(fixture.protocol, &ping, "127.0.0.2", 8002);
    
    // Verify node 3 is SUSPECT
    cluster_member_t *m3 = cluster_view_get(&fixture.cluster_view, 3);
    assert(m3 != NULL && m3->status == NODE_STATUS_SUSPECT);
    cluster_view_release(&fixture.cluster_view);
    
    // Verify node 4 is DEAD
    cluster_member_t *m4 = cluster_view_get(&fixture.cluster_view, 4);
    assert(m4 != NULL && m4->status == NODE_STATUS_DEAD);
    cluster_view_release(&fixture.cluster_view);
    
    // Verify node 7 was discovered
    cluster_member_t *m7 = cluster_view_get(&fixture.cluster_view, 7);
    assert(m7 != NULL && m7->status == NODE_STATUS_ALIVE);
    cluster_view_release(&fixture.cluster_view);
    
    // Verify callbacks invoked
    assert(fixture.suspect_count == 1);  // Node 3
    assert(fixture.dead_count == 1);     // Node 4
    assert(fixture.alive_count == 1);    // Node 7
    
    // Verify ACK was sent
    assert(fixture.send_count == 1);
    assert(fixture.last_sent_msg.msg_type == GOSSIP_MSG_ACK);
    
    teardown_fixture(&fixture);
    printf("✅ Test passed\n");
    return 0;
}

static int test_ack_with_cluster_state()
{
    printf("\n=== Test: ACK with Piggybacked Cluster State ===\n");
    
    test_fixture_t fixture;
    setup_fixture(&fixture);
    
    // Add node 2 to cluster
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
    
    // Create ACK with piggybacked updates
    gossip_message_t ack = {
        .version = 1,
        .msg_type = GOSSIP_MSG_ACK,
        .sender_id = 2,
        .sequence_num = 1,
        .num_updates = 2
    };
    
    // Piggybacked update: Node 3 exists
    ack.updates[0].node_id = 3;
    ack.updates[0].node_type = NODE_TYPE_WORKER;
    ack.updates[0].status = NODE_STATUS_ALIVE;
    ack.updates[0].incarnation = 0;
    ack.updates[0].gossip_port = 8003;
    ack.updates[0].data_port = 9003;
    safe_strncpy(ack.updates[0].ip_address, "127.0.0.3", MAX_IP_LEN);
    
    // Piggybacked update: Node 4 exists
    ack.updates[1].node_id = 4;
    ack.updates[1].node_type = NODE_TYPE_WORKER;
    ack.updates[1].status = NODE_STATUS_ALIVE;
    ack.updates[1].incarnation = 0;
    ack.updates[1].gossip_port = 8004;
    ack.updates[1].data_port = 9004;
    safe_strncpy(ack.updates[1].ip_address, "127.0.0.4", MAX_IP_LEN);
    
    // Handle ACK
    gossip_protocol_handle_message(fixture.protocol, &ack, "127.0.0.2", 8001);
    
    // Verify nodes 3 and 4 were discovered
    cluster_member_t *m3 = cluster_view_get(&fixture.cluster_view, 3);
    assert(m3 != NULL && m3->status == NODE_STATUS_ALIVE);
    cluster_view_release(&fixture.cluster_view);
    
    cluster_member_t *m4 = cluster_view_get(&fixture.cluster_view, 4);
    assert(m4 != NULL && m4->status == NODE_STATUS_ALIVE);
    cluster_view_release(&fixture.cluster_view);
    
    assert(fixture.alive_count == 2);  // Nodes 3 and 4
    
    teardown_fixture(&fixture);
    printf("✅ Test passed\n");
    return 0;
}

static int test_suspect_from_peer()
{
    printf("\n=== Test: SUSPECT Message from Peer ===\n");
    
    test_fixture_t fixture;
    setup_fixture(&fixture);
    
    // Add node 2 as ALIVE
    cluster_member_t peer = {
        .node_id = 2,
        .node_type = NODE_TYPE_ROUTER,
        .status = NODE_STATUS_ALIVE,
        .incarnation = 5,
        .gossip_port = 8001,
        .data_port = 9001
    };
    safe_strncpy(peer.ip_address, "127.0.0.2", MAX_IP_LEN);
    cluster_view_add(&fixture.cluster_view, &peer);
    
    // Node 3 sends SUSPECT about node 2
    gossip_message_t suspect = {
        .version = 1,
        .msg_type = GOSSIP_MSG_SUSPECT,
        .sender_id = 3,
        .sequence_num = 1,
        .num_updates = 1
    };
    
    suspect.updates[0].node_id = 2;
    suspect.updates[0].status = NODE_STATUS_SUSPECT;
    suspect.updates[0].incarnation = 5;  // Same incarnation
    
    gossip_protocol_handle_message(fixture.protocol, &suspect, "127.0.0.3", 8003);
    
    // Verify node 2 is now SUSPECT
    cluster_member_t *m2 = cluster_view_get(&fixture.cluster_view, 2);
    assert(m2 != NULL && m2->status == NODE_STATUS_SUSPECT);
    cluster_view_release(&fixture.cluster_view);
    
    assert(fixture.suspect_count == 1);
    assert(fixture.last_suspect_node == 2);
    
    teardown_fixture(&fixture);
    printf("✅ Test passed\n");
    return 0;
}

static int test_alive_refutation_from_peer()
{
    printf("\n=== Test: ALIVE Refutation from Peer ===\n");
    
    test_fixture_t fixture;
    setup_fixture(&fixture);
    
    // Add node 2 as SUSPECT
    cluster_member_t peer = {
        .node_id = 2,
        .node_type = NODE_TYPE_ROUTER,
        .status = NODE_STATUS_SUSPECT,
        .incarnation = 5,
        .gossip_port = 8001,
        .data_port = 9001
    };
    safe_strncpy(peer.ip_address, "127.0.0.2", MAX_IP_LEN);
    cluster_view_add(&fixture.cluster_view, &peer);
    
    // Node 2 sends ALIVE with higher incarnation
    gossip_message_t alive = {
        .version = 1,
        .msg_type = GOSSIP_MSG_ALIVE,
        .sender_id = 2,
        .sequence_num = 1,
        .num_updates = 1
    };
    
    alive.updates[0].node_id = 2;
    alive.updates[0].node_type = NODE_TYPE_ROUTER;
    alive.updates[0].status = NODE_STATUS_ALIVE;
    alive.updates[0].incarnation = 6;  // Higher incarnation
    alive.updates[0].gossip_port = 8001;
    alive.updates[0].data_port = 9001;
    safe_strncpy(alive.updates[0].ip_address, "127.0.0.2", MAX_IP_LEN);
    
    gossip_protocol_handle_message(fixture.protocol, &alive, "127.0.0.2", 8001);
    
    // Verify node 2 is now ALIVE
    cluster_member_t *m2 = cluster_view_get(&fixture.cluster_view, 2);
    assert(m2 != NULL && m2->status == NODE_STATUS_ALIVE);
    assert(m2->incarnation == 6);
    cluster_view_release(&fixture.cluster_view);
    
    assert(fixture.alive_count == 1);
    assert(fixture.last_alive_node == 2);
    
    teardown_fixture(&fixture);
    printf("✅ Test passed\n");
    return 0;
}

static int test_dead_message()
{
    printf("\n=== Test: DEAD Message ===\n");
    
    test_fixture_t fixture;
    setup_fixture(&fixture);
    
    // Add node 2 as SUSPECT
    cluster_member_t peer = {
        .node_id = 2,
        .node_type = NODE_TYPE_ROUTER,
        .status = NODE_STATUS_SUSPECT,
        .incarnation = 0,
        .gossip_port = 8001,
        .data_port = 9001
    };
    safe_strncpy(peer.ip_address, "127.0.0.2", MAX_IP_LEN);
    cluster_view_add(&fixture.cluster_view, &peer);
    
    // Node 3 sends DEAD about node 2
    gossip_message_t dead = {
        .version = 1,
        .msg_type = GOSSIP_MSG_DEAD,
        .sender_id = 3,
        .sequence_num = 1,
        .num_updates = 1
    };
    
    dead.updates[0].node_id = 2;
    dead.updates[0].status = NODE_STATUS_DEAD;
    dead.updates[0].incarnation = 0;
    
    gossip_protocol_handle_message(fixture.protocol, &dead, "127.0.0.3", 8003);
    
    // Verify node 2 is now DEAD
    cluster_member_t *m2 = cluster_view_get(&fixture.cluster_view, 2);
    assert(m2 != NULL && m2->status == NODE_STATUS_DEAD);
    cluster_view_release(&fixture.cluster_view);
    
    assert(fixture.dead_count == 1);
    assert(fixture.last_dead_node == 2);
    
    teardown_fixture(&fixture);
    printf("✅ Test passed\n");
    return 0;
}

static int test_swim_round_piggybacks_cluster_state()
{
    printf("\n=== Test: SWIM Round Piggybacks Cluster State ===\n");
    
    test_fixture_t fixture;
    setup_fixture(&fixture);
    
    // Add multiple peers to cluster
    for (int i = 2; i <= 5; i++) {
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
    
    // Run SWIM round (should send PING)
    gossip_protocol_run_swim_round(fixture.protocol);
    
    // Verify PING was sent
    assert(fixture.send_count == 1);
    assert(fixture.last_sent_msg.msg_type == GOSSIP_MSG_PING);
    
    // Verify PING includes piggybacked updates
    printf("PING includes %d piggybacked updates\n", fixture.last_sent_msg.num_updates);
    assert(fixture.last_sent_msg.num_updates > 0);
    
    // Verify sender ID
    assert(fixture.last_sent_msg.sender_id == 1);
    
    teardown_fixture(&fixture);
    printf("✅ Test passed\n");
    return 0;
}

static int test_suspect_recovery_via_ack()
{
    printf("\n=== Test: SUSPECT Recovery via ACK ===\n");
    
    test_fixture_t fixture;
    setup_fixture(&fixture);
    
    // Add node 2 as SUSPECT
    cluster_member_t peer = {
        .node_id = 2,
        .node_type = NODE_TYPE_ROUTER,
        .status = NODE_STATUS_SUSPECT,
        .incarnation = 0,
        .gossip_port = 8001,
        .data_port = 9001
    };
    safe_strncpy(peer.ip_address, "127.0.0.2", MAX_IP_LEN);
    cluster_view_add(&fixture.cluster_view, &peer);
    
    // Node 2 sends ACK (proving it's alive)
    gossip_message_t ack = {
        .version = 1,
        .msg_type = GOSSIP_MSG_ACK,
        .sender_id = 2,
        .sequence_num = 1,
        .num_updates = 0
    };
    
    gossip_protocol_handle_message(fixture.protocol, &ack, "127.0.0.2", 8001);
    
    // Verify node 2 recovered to ALIVE
    cluster_member_t *m2 = cluster_view_get(&fixture.cluster_view, 2);
    assert(m2 != NULL && m2->status == NODE_STATUS_ALIVE);
    cluster_view_release(&fixture.cluster_view);
    
    teardown_fixture(&fixture);
    printf("✅ Test passed\n");
    return 0;
}

static int test_ignore_messages_from_self()
{
    printf("\n=== Test: Ignore Messages from Self ===\n");
    
    test_fixture_t fixture;
    setup_fixture(&fixture);
    
    // Create PING from ourselves (node 1)
    gossip_message_t ping = {
        .version = 1,
        .msg_type = GOSSIP_MSG_PING,
        .sender_id = 1,  // Our own ID
        .sequence_num = 1,
        .num_updates = 0
    };
    
    gossip_protocol_handle_message(fixture.protocol, &ping, "127.0.0.1", 8000);
    
    // Should be ignored - no ACK sent
    assert(fixture.send_count == 0);
    assert(fixture.alive_count == 0);
    
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
    printf("  Message Handler Tests\n");
    printf("===========================================\n");
    
    int failed = 0;
    
    if (test_ping_with_multiple_updates() != 0) failed++;
    if (test_ack_with_cluster_state() != 0) failed++;
    if (test_suspect_from_peer() != 0) failed++;
    if (test_alive_refutation_from_peer() != 0) failed++;
    if (test_dead_message() != 0) failed++;
    if (test_swim_round_piggybacks_cluster_state() != 0) failed++;
    if (test_suspect_recovery_via_ack() != 0) failed++;
    if (test_ignore_messages_from_self() != 0) failed++;
    
    printf("\n===========================================\n");
    printf("  Summary\n");
    printf("===========================================\n");
    if (failed == 0) {
        printf("✅ All message handler tests passed!\n");
        return 0;
    }
    printf("❌ %d test(s) failed\n", failed);
    return 1;
}
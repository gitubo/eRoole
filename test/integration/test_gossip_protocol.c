// test/integration/test_gossip_protocol.c
// End-to-end integration test with real UDP transport

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include "roole/gossip/gossip_engine.h"
#include "roole/cluster/cluster_view.h"

typedef struct {
    gossip_engine_t *engine;
    cluster_view_t cluster_view;
    
    int join_count;
    int leave_count;
    int failed_count;
} test_node_t;

static void member_event_callback(node_id_t node_id,
                                 node_type_t node_type,
                                 const char *ip_address,
                                 uint16_t data_port,
                                 const char *event_type,
                                 void *user_data)
{
    test_node_t *node = (test_node_t*)user_data;
    
    printf("[NODE CALLBACK] Node %u: %s (type=%d, ip=%s, data_port=%u)\n",
           node_id, event_type, node_type, ip_address, data_port);
    
    if (strcmp(event_type, MEMBER_EVENT_JOIN) == 0) {
        node->join_count++;
    } else if (strcmp(event_type, MEMBER_EVENT_LEAVE) == 0) {
        node->leave_count++;
    } else if (strcmp(event_type, MEMBER_EVENT_FAILED) == 0) {
        node->failed_count++;
    }
}

static int test_two_node_gossip()
{
    printf("\n=== Integration Test: Two Node Gossip ===\n");
    
    test_node_t node1, node2;
    memset(&node1, 0, sizeof(node1));
    memset(&node2, 0, sizeof(node2));
    
    // Initialize cluster views
    cluster_view_init(&node1.cluster_view, 100);
    cluster_view_init(&node2.cluster_view, 100);
    
    gossip_config_t config = gossip_default_config();
    config.protocol_period_ms = 500; // Faster for testing
    
    // Create node 1
    node1.engine = gossip_engine_create(
        1, NODE_TYPE_ROUTER, "127.0.0.1", 10001, 11001,
        &config, &node1.cluster_view,
        member_event_callback, &node1
    );
    assert(node1.engine != NULL);
    
    // Create node 2
    node2.engine = gossip_engine_create(
        2, NODE_TYPE_ROUTER, "127.0.0.1", 10002, 11002,
        &config, &node2.cluster_view,
        member_event_callback, &node2
    );
    assert(node2.engine != NULL);
    
    // Start both engines
    assert(gossip_engine_start(node1.engine) == 0);
    assert(gossip_engine_start(node2.engine) == 0);
    
    // Node 1 announces join
    gossip_engine_announce_join(node1.engine);
    
    // Node 2 joins via node 1 seed
    gossip_engine_add_seed(node2.engine, "127.0.0.1", 10001);
    gossip_engine_announce_join(node2.engine);
    
    printf("Waiting for nodes to discover each other...\n");
    sleep(3);
    
    // Verify both nodes discovered each other
    printf("\n--- Node 1 Cluster View ---\n");
    cluster_view_dump(&node1.cluster_view, "Node 1");
    
    printf("\n--- Node 2 Cluster View ---\n");
    cluster_view_dump(&node2.cluster_view, "Node 2");
    
    // Verify node 1 knows about node 2
    cluster_member_t *member = cluster_view_get(&node1.cluster_view, 2);
    assert(member != NULL);
    assert(member->status == NODE_STATUS_ALIVE);
    cluster_view_release(&node1.cluster_view);
    
    // Verify node 2 knows about node 1
    member = cluster_view_get(&node2.cluster_view, 1);
    assert(member != NULL);
    assert(member->status == NODE_STATUS_ALIVE);
    cluster_view_release(&node2.cluster_view);
    
    // Verify callbacks were invoked
    assert(node1.join_count >= 1); // Discovered node 2
    assert(node2.join_count >= 1); // Discovered node 1
    
    printf("\nNode 1 events: join=%d leave=%d failed=%d\n",
           node1.join_count, node1.leave_count, node1.failed_count);
    printf("Node 2 events: join=%d leave=%d failed=%d\n",
           node2.join_count, node2.leave_count, node2.failed_count);
    
    // Cleanup
    gossip_engine_shutdown(node1.engine);
    gossip_engine_shutdown(node2.engine);
    
    cluster_view_destroy(&node1.cluster_view);
    cluster_view_destroy(&node2.cluster_view);
    
    printf("✅ Test passed\n");
    return 0;
}

static int test_failure_detection()
{
    printf("\n=== Integration Test: Failure Detection ===\n");
    
    test_node_t node1, node2;
    memset(&node1, 0, sizeof(node1));
    memset(&node2, 0, sizeof(node2));
    
    cluster_view_init(&node1.cluster_view, 100);
    cluster_view_init(&node2.cluster_view, 100);
    
    gossip_config_t config = gossip_default_config();
    config.protocol_period_ms = 500;
    config.ack_timeout_ms = 300;
    config.dead_timeout_ms = 1000;
    
    // Create node 1
    node1.engine = gossip_engine_create(
        1, NODE_TYPE_ROUTER, "127.0.0.1", 10003, 11003,
        &config, &node1.cluster_view,
        member_event_callback, &node1
    );
    assert(node1.engine != NULL);
    
    // Create node 2
    node2.engine = gossip_engine_create(
        2, NODE_TYPE_ROUTER, "127.0.0.1", 10004, 11004,
        &config, &node2.cluster_view,
        member_event_callback, &node2
    );
    assert(node2.engine != NULL);
    
    // Start both engines
    assert(gossip_engine_start(node1.engine) == 0);
    assert(gossip_engine_start(node2.engine) == 0);
    
    gossip_engine_announce_join(node1.engine);
    gossip_engine_add_seed(node2.engine, "127.0.0.1", 10003);
    gossip_engine_announce_join(node2.engine);
    
    printf("Waiting for nodes to discover each other...\n");
    sleep(2);
    
    // Verify both nodes are ALIVE
    cluster_member_t *member = cluster_view_get(&node1.cluster_view, 2);
    assert(member != NULL);
    assert(member->status == NODE_STATUS_ALIVE);
    cluster_view_release(&node1.cluster_view);
    
    printf("\n--- Shutting down Node 2 (simulating crash) ---\n");
    gossip_engine_shutdown(node2.engine);
    
    printf("Waiting for Node 1 to detect failure...\n");
    sleep(4); // Wait for SUSPECT + DEAD timeouts
    
    // Verify node 1 detected node 2 as failed
    member = cluster_view_get(&node1.cluster_view, 2);
    assert(member != NULL);
    printf("Node 2 status from Node 1's view: %d (0=ALIVE, 1=SUSPECT, 2=DEAD)\n",
           member->status);
    assert(member->status == NODE_STATUS_SUSPECT || member->status == NODE_STATUS_DEAD);
    cluster_view_release(&node1.cluster_view);
    
    // Verify callbacks were invoked
    printf("\nNode 1 failure events: failed=%d leave=%d\n",
           node1.failed_count, node1.leave_count);
    assert(node1.failed_count >= 1 || node1.leave_count >= 1);
    
    // Cleanup
    gossip_engine_shutdown(node1.engine);
    cluster_view_destroy(&node1.cluster_view);
    cluster_view_destroy(&node2.cluster_view);
    
    printf("✅ Test passed\n");
    return 0;
}

static int test_graceful_leave()
{
    printf("\n=== Integration Test: Graceful Leave ===\n");
    
    test_node_t node1, node2;
    memset(&node1, 0, sizeof(node1));
    memset(&node2, 0, sizeof(node2));
    
    cluster_view_init(&node1.cluster_view, 100);
    cluster_view_init(&node2.cluster_view, 100);
    
    gossip_config_t config = gossip_default_config();
    config.protocol_period_ms = 500;
    
    // Create and start both nodes
    node1.engine = gossip_engine_create(
        1, NODE_TYPE_ROUTER, "127.0.0.1", 10005, 11005,
        &config, &node1.cluster_view,
        member_event_callback, &node1
    );
    
    node2.engine = gossip_engine_create(
        2, NODE_TYPE_ROUTER, "127.0.0.1", 10006, 11006,
        &config, &node2.cluster_view,
        member_event_callback, &node2
    );
    
    assert(gossip_engine_start(node1.engine) == 0);
    assert(gossip_engine_start(node2.engine) == 0);
    
    gossip_engine_announce_join(node1.engine);
    gossip_engine_add_seed(node2.engine, "127.0.0.1", 10005);
    gossip_engine_announce_join(node2.engine);
    
    sleep(2);
    
    // Verify both nodes are ALIVE
    cluster_member_t *member = cluster_view_get(&node1.cluster_view, 2);
    assert(member != NULL);
    assert(member->status == NODE_STATUS_ALIVE);
    cluster_view_release(&node1.cluster_view);
    
    printf("\n--- Node 2 announcing graceful leave ---\n");
    gossip_engine_leave(node2.engine);
    
    printf("Waiting for Node 1 to receive LEAVE...\n");
    sleep(2);
    
    // Verify node 1 received LEAVE
    member = cluster_view_get(&node1.cluster_view, 2);
    assert(member != NULL);
    printf("Node 2 status: %d (should be DEAD=2)\n", member->status);
    assert(member->status == NODE_STATUS_DEAD);
    cluster_view_release(&node1.cluster_view);
    
    // Verify leave callback was invoked
    printf("\nNode 1 leave events: leave=%d\n", node1.leave_count);
    assert(node1.leave_count >= 1);
    
    // Cleanup
    gossip_engine_shutdown(node2.engine);
    gossip_engine_shutdown(node1.engine);
    
    cluster_view_destroy(&node1.cluster_view);
    cluster_view_destroy(&node2.cluster_view);
    
    printf("✅ Test passed\n");
    return 0;
}

// ============================================================================
// TEST RUNNER
// ============================================================================

int main(void)
{
    printf("===========================================\n");
    printf("  Gossip Protocol Integration Tests\n");
    printf("===========================================\n");
    
    int failed = 0;
    
    if (test_two_node_gossip() != 0) failed++;
    if (test_failure_detection() != 0) failed++;
    if (test_graceful_leave() != 0) failed++;
    
    printf("\n===========================================\n");
    printf("  Summary\n");
    printf("===========================================\n");
    if (failed == 0) {
        printf("✅ All integration tests passed!\n");
        return 0;
    }
    printf("❌ %d test(s) failed\n", failed);
    return 1;
}
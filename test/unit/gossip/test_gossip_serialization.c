// test/unit/gossip/test_gossip_serialization.c
// Tests for gossip message serialization/deserialization

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "roole/gossip/gossip_types.h"
#include "roole/core/common.h"

// ============================================================================
// TEST CASES
// ============================================================================

static int test_serialize_deserialize_roundtrip()
{
    printf("\n=== Test: Serialize/Deserialize Roundtrip ===\n");
    
    // Create original message
    gossip_message_t original = {
        .version = 1,
        .msg_type = GOSSIP_MSG_PING,
        .flags = 0,
        .sender_id = 42,
        .sequence_num = 12345,
        .num_updates = 2
    };
    
    // Update 1
    original.updates[0].node_id = 10;
    original.updates[0].node_type = NODE_TYPE_ROUTER;
    original.updates[0].status = NODE_STATUS_ALIVE;
    original.updates[0].incarnation = 5;
    original.updates[0].gossip_port = 8001;
    original.updates[0].data_port = 9001;
    safe_strncpy(original.updates[0].ip_address, "192.168.1.1", MAX_IP_LEN);
    original.updates[0].timestamp_ms = 1000000;
    
    // Update 2
    original.updates[1].node_id = 20;
    original.updates[1].node_type = NODE_TYPE_WORKER;
    original.updates[1].status = NODE_STATUS_SUSPECT;
    original.updates[1].incarnation = 3;
    original.updates[1].gossip_port = 8002;
    original.updates[1].data_port = 9002;
    safe_strncpy(original.updates[1].ip_address, "192.168.1.2", MAX_IP_LEN);
    original.updates[1].timestamp_ms = 2000000;
    
    // Serialize
    uint8_t buffer[GOSSIP_MAX_PAYLOAD_SIZE];
    ssize_t size = gossip_message_serialize(&original, buffer, sizeof(buffer));
    
    assert(size > 0);
    printf("Serialized size: %zd bytes\n", size);
    
    // Deserialize
    gossip_message_t deserialized;
    int rc = gossip_message_deserialize(buffer, size, &deserialized);
    assert(rc == 0);
    
    // Verify header
    assert(deserialized.version == original.version);
    assert(deserialized.msg_type == original.msg_type);
    assert(deserialized.sender_id == original.sender_id);
    assert(deserialized.sequence_num == original.sequence_num);
    assert(deserialized.num_updates == original.num_updates);
    
    // Verify update 1
    assert(deserialized.updates[0].node_id == 10);
    assert(deserialized.updates[0].node_type == NODE_TYPE_ROUTER);
    assert(deserialized.updates[0].status == NODE_STATUS_ALIVE);
    assert(deserialized.updates[0].incarnation == 5);
    assert(deserialized.updates[0].gossip_port == 8001);
    assert(deserialized.updates[0].data_port == 9001);
    assert(strcmp(deserialized.updates[0].ip_address, "192.168.1.1") == 0);
    assert(deserialized.updates[0].timestamp_ms == 1000000);
    
    // Verify update 2
    assert(deserialized.updates[1].node_id == 20);
    assert(deserialized.updates[1].node_type == NODE_TYPE_WORKER);
    assert(deserialized.updates[1].status == NODE_STATUS_SUSPECT);
    assert(deserialized.updates[1].incarnation == 3);
    
    printf("✅ Test passed\n");
    return 0;
}

static int test_serialize_max_updates()
{
    printf("\n=== Test: Serialize Maximum Updates ===\n");
    
    gossip_message_t msg = {
        .version = 1,
        .msg_type = GOSSIP_MSG_ACK,
        .sender_id = 1,
        .sequence_num = 1,
        .num_updates = GOSSIP_MAX_PIGGYBACK_UPDATES
    };
    
    // Fill all update slots
    for (int i = 0; i < GOSSIP_MAX_PIGGYBACK_UPDATES; i++) {
        msg.updates[i].node_id = 100 + i;
        msg.updates[i].status = NODE_STATUS_ALIVE;
        msg.updates[i].incarnation = i;
        snprintf(msg.updates[i].ip_address, MAX_IP_LEN, "10.0.0.%d", i);
    }
    
    uint8_t buffer[GOSSIP_MAX_PAYLOAD_SIZE];
    ssize_t size = gossip_message_serialize(&msg, buffer, sizeof(buffer));
    
    assert(size > 0);
    printf("Serialized %d updates: %zd bytes\n", GOSSIP_MAX_PIGGYBACK_UPDATES, size);
    
    // Deserialize
    gossip_message_t deserialized;
    int rc = gossip_message_deserialize(buffer, size, &deserialized);
    assert(rc == 0);
    assert(deserialized.num_updates == GOSSIP_MAX_PIGGYBACK_UPDATES);
    
    // Verify first and last update
    assert(deserialized.updates[0].node_id == 100);
    assert(deserialized.updates[GOSSIP_MAX_PIGGYBACK_UPDATES-1].node_id == 100 + GOSSIP_MAX_PIGGYBACK_UPDATES - 1);
    
    printf("✅ Test passed\n");
    return 0;
}

static int test_serialize_no_updates()
{
    printf("\n=== Test: Serialize Message with No Updates ===\n");
    
    gossip_message_t msg = {
        .version = 1,
        .msg_type = GOSSIP_MSG_PING,
        .sender_id = 5,
        .sequence_num = 999,
        .num_updates = 0
    };
    
    uint8_t buffer[GOSSIP_MAX_PAYLOAD_SIZE];
    ssize_t size = gossip_message_serialize(&msg, buffer, sizeof(buffer));
    
    assert(size > 0);
    printf("Serialized message with 0 updates: %zd bytes\n", size);
    
    gossip_message_t deserialized;
    int rc = gossip_message_deserialize(buffer, size, &deserialized);
    assert(rc == 0);
    assert(deserialized.num_updates == 0);
    assert(deserialized.sender_id == 5);
    
    printf("✅ Test passed\n");
    return 0;
}

static int test_deserialize_truncated_message()
{
    printf("\n=== Test: Deserialize Truncated Message ===\n");
    
    gossip_message_t msg;
    uint8_t truncated[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    
    int rc = gossip_message_deserialize(truncated, sizeof(truncated), &msg);
    
    // Should fail - message too short
    assert(rc != 0);
    printf("Correctly rejected truncated message\n");
    
    printf("✅ Test passed\n");
    return 0;
}

static int test_deserialize_invalid_version()
{
    printf("\n=== Test: Deserialize Invalid Version ===\n");
    
    uint8_t buffer[GOSSIP_MAX_PAYLOAD_SIZE];
    memset(buffer, 0, sizeof(buffer));
    
    buffer[0] = 99;  // Invalid version (should be 1)
    buffer[1] = GOSSIP_MSG_PING;
    
    gossip_message_t msg;
    int rc = gossip_message_deserialize(buffer, 100, &msg);
    
    // Should fail - unsupported version
    assert(rc != 0);
    printf("Correctly rejected invalid version\n");
    
    printf("✅ Test passed\n");
    return 0;
}

static int test_serialize_buffer_too_small()
{
    printf("\n=== Test: Serialize with Small Buffer ===\n");
    
    gossip_message_t msg = {
        .version = 1,
        .msg_type = GOSSIP_MSG_PING,
        .sender_id = 1,
        .sequence_num = 1,
        .num_updates = 5
    };
    
    // Buffer too small for all updates
    uint8_t small_buffer[50];
    ssize_t size = gossip_message_serialize(&msg, small_buffer, sizeof(small_buffer));
    
    // Should still succeed but truncate updates
    assert(size > 0);
    assert(size <= (ssize_t)sizeof(small_buffer));
    
    printf("Serialization truncated gracefully: %zd bytes\n", size);
    
    printf("✅ Test passed\n");
    return 0;
}

static int test_bootstrap_response_roundtrip()
{
    printf("\n=== Test: Bootstrap Response Roundtrip ===\n");
    
    gossip_bootstrap_response_t original = {
        .num_routers = 3
    };
    
    // Router 1
    original.routers[0].node_id = 1;
    safe_strncpy(original.routers[0].gossip_addr, "192.168.1.1:8000", MAX_CONFIG_STRING);
    safe_strncpy(original.routers[0].data_addr, "192.168.1.1:9000", MAX_CONFIG_STRING);
    
    // Router 2
    original.routers[1].node_id = 2;
    safe_strncpy(original.routers[1].gossip_addr, "192.168.1.2:8000", MAX_CONFIG_STRING);
    safe_strncpy(original.routers[1].data_addr, "192.168.1.2:9000", MAX_CONFIG_STRING);
    
    // Router 3
    original.routers[2].node_id = 3;
    safe_strncpy(original.routers[2].gossip_addr, "192.168.1.3:8000", MAX_CONFIG_STRING);
    safe_strncpy(original.routers[2].data_addr, "192.168.1.3:9000", MAX_CONFIG_STRING);
    
    // Serialize
    uint8_t buffer[2048];
    ssize_t size = gossip_serialize_bootstrap_response(&original, buffer, sizeof(buffer));
    
    assert(size > 0);
    printf("Serialized bootstrap response: %zd bytes\n", size);
    
    // Deserialize
    gossip_bootstrap_response_t deserialized;
    int rc = gossip_deserialize_bootstrap_response(buffer, size, &deserialized);
    assert(rc == 0);
    
    assert(deserialized.num_routers == 3);
    assert(deserialized.routers[0].node_id == 1);
    assert(strcmp(deserialized.routers[0].gossip_addr, "192.168.1.1:8000") == 0);
    assert(strcmp(deserialized.routers[2].data_addr, "192.168.1.3:9000") == 0);
    
    printf("✅ Test passed\n");
    return 0;
}

static int test_message_size_calculation()
{
    printf("\n=== Test: Message Size Calculation ===\n");
    
    gossip_message_t msg = {
        .version = 1,
        .msg_type = GOSSIP_MSG_PING,
        .sender_id = 1,
        .num_updates = 3
    };
    
    size_t expected_size = gossip_message_serialized_size(&msg);
    
    uint8_t buffer[GOSSIP_MAX_PAYLOAD_SIZE];
    ssize_t actual_size = gossip_message_serialize(&msg, buffer, sizeof(buffer));
    
    assert(actual_size > 0);
    assert((size_t)actual_size == expected_size);
    
    printf("Calculated size: %zu, Actual size: %zd\n", expected_size, actual_size);
    printf("✅ Test passed\n");
    return 0;
}

// ============================================================================
// TEST RUNNER
// ============================================================================

int main(void)
{
    printf("===========================================\n");
    printf("  Gossip Serialization Tests\n");
    printf("===========================================\n");
    
    int failed = 0;
    
    if (test_serialize_deserialize_roundtrip() != 0) failed++;
    if (test_serialize_max_updates() != 0) failed++;
    if (test_serialize_no_updates() != 0) failed++;
    if (test_deserialize_truncated_message() != 0) failed++;
    if (test_deserialize_invalid_version() != 0) failed++;
    if (test_serialize_buffer_too_small() != 0) failed++;
    if (test_bootstrap_response_roundtrip() != 0) failed++;
    if (test_message_size_calculation() != 0) failed++;
    
    printf("\n===========================================\n");
    printf("  Summary\n");
    printf("===========================================\n");
    if (failed == 0) {
        printf("✅ All serialization tests passed!\n");
        return 0;
    }
    printf("❌ %d test(s) failed\n", failed);
    return 1;
}
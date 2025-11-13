// test/unit/rpc/test_rpc_serialization.c
// Unit tests for RPC message serialization

#include "roole/rpc/rpc_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

void test_pack_unpack_request(void) {
    printf("Test: Pack/Unpack Request... ");
    
    uint8_t buffer[256];
    const char *payload = "test_payload";
    size_t payload_len = strlen(payload);
    
    size_t packed_len = rpc_pack_message(buffer, 123, 456,
                                         RPC_TYPE_REQUEST, RPC_STATUS_SUCCESS,
                                         FUNC_ID_ADD, (const uint8_t*)payload, payload_len);
    
    assert(packed_len == RPC_HEADER_SIZE + payload_len);
    
    rpc_header_t header;
    assert(rpc_unpack_header(buffer, &header) == 0);
    
    assert(header.total_len == packed_len);
    assert(header.request_id == 456);
    assert(header.sender_id == 123);
    assert(header.type_and_status.fields.type == RPC_TYPE_REQUEST);
    assert(header.type_and_status.fields.status == RPC_STATUS_SUCCESS);
    assert(header.func_id == FUNC_ID_ADD);
    
    // Verify payload
    assert(memcmp(buffer + RPC_HEADER_SIZE, payload, payload_len) == 0);
    
    printf("✓\n");
}

void test_pack_unpack_response(void) {
    printf("Test: Pack/Unpack Response... ");
    
    uint8_t buffer[256];
    uint32_t response_data = 0x12345678;
    
    size_t packed_len = rpc_pack_message(buffer, 999, 777,
                                         RPC_TYPE_RESPONSE, RPC_STATUS_SUCCESS,
                                         FUNC_ID_EXECUTE_DAG,
                                         (const uint8_t*)&response_data, sizeof(response_data));
    
    assert(packed_len == RPC_HEADER_SIZE + sizeof(response_data));
    
    rpc_header_t header;
    assert(rpc_unpack_header(buffer, &header) == 0);
    
    assert(header.total_len == packed_len);
    assert(header.request_id == 777);
    assert(header.sender_id == 999);
    assert(header.type_and_status.fields.type == RPC_TYPE_RESPONSE);
    assert(header.type_and_status.fields.status == RPC_STATUS_SUCCESS);
    assert(header.func_id == FUNC_ID_EXECUTE_DAG);
    
    uint32_t received_data;
    memcpy(&received_data, buffer + RPC_HEADER_SIZE, sizeof(received_data));
    assert(received_data == response_data);
    
    printf("✓\n");
}

void test_pack_empty_payload(void) {
    printf("Test: Pack/Unpack Empty Payload... ");
    
    uint8_t buffer[256];
    
    size_t packed_len = rpc_pack_message(buffer, 1, 2,
                                         RPC_TYPE_REQUEST, RPC_STATUS_SUCCESS,
                                         FUNC_ID_HEARTBEAT, NULL, 0);
    
    assert(packed_len == RPC_HEADER_SIZE);
    
    rpc_header_t header;
    assert(rpc_unpack_header(buffer, &header) == 0);
    
    assert(header.total_len == RPC_HEADER_SIZE);
    assert(header.request_id == 2);
    
    printf("✓\n");
}

void test_status_codes(void) {
    printf("Test: Various Status Codes... ");
    
    uint8_t buffer[256];
    rpc_status_t statuses[] = {
        RPC_STATUS_SUCCESS,
        RPC_STATUS_BAD_ARGUMENT,
        RPC_STATUS_FUNC_NOT_FOUND,
        RPC_STATUS_INTERNAL_ERROR,
        RPC_STATUS_TIMEOUT
    };
    
    for (size_t i = 0; i < sizeof(statuses) / sizeof(statuses[0]); i++) {
        rpc_pack_message(buffer, 0, 0, RPC_TYPE_RESPONSE, statuses[i],
                        0, NULL, 0);
        
        rpc_header_t header;
        rpc_unpack_header(buffer, &header);
        
        assert(header.type_and_status.fields.status == statuses[i]);
    }
    
    printf("✓\n");
}

void test_large_payload(void) {
    printf("Test: Large Payload... ");
    
    size_t payload_size = 1024 * 64; // 64KB
    uint8_t *payload = malloc(payload_size);
    uint8_t *buffer = malloc(RPC_HEADER_SIZE + payload_size);
    
    // Fill with pattern
    for (size_t i = 0; i < payload_size; i++) {
        payload[i] = (uint8_t)(i % 256);
    }
    
    size_t packed_len = rpc_pack_message(buffer, 0, 0, RPC_TYPE_REQUEST,
                                         RPC_STATUS_SUCCESS, 0,
                                         payload, payload_size);
    
    assert(packed_len == RPC_HEADER_SIZE + payload_size);
    
    rpc_header_t header;
    assert(rpc_unpack_header(buffer, &header) == 0);
    
    assert(header.total_len == packed_len);
    assert(memcmp(buffer + RPC_HEADER_SIZE, payload, payload_size) == 0);
    
    free(payload);
    free(buffer);
    
    printf("✓\n");
}

void test_invalid_header(void) {
    printf("Test: Invalid Header Detection... ");
    
    uint8_t buffer[RPC_HEADER_SIZE] = {0};
    
    // Set invalid total_len (less than header size)
    uint32_t invalid_len = 5;
    memcpy(buffer, &invalid_len, 4);
    
    rpc_header_t header;
    assert(rpc_unpack_header(buffer, &header) < 0);
    
    printf("✓\n");
}

int main(void) {
    printf("=================================\n");
    printf("  RPC Serialization Unit Tests\n");
    printf("=================================\n\n");
    
    test_pack_unpack_request();
    test_pack_unpack_response();
    test_pack_empty_payload();
    test_status_codes();
    test_large_payload();
    test_invalid_header();
    
    printf("\n=================================\n");
    printf("  All tests passed ✓\n");
    printf("=================================\n");
    
    return 0;
}
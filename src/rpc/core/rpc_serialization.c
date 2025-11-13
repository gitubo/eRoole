#include "roole/rpc/rpc_types.h"
#include "roole/core/common.h"

size_t rpc_pack_message(uint8_t *buffer, node_id_t node_id, uint32_t request_id, 
                         uint8_t type, uint8_t status, uint8_t func_id, 
                         const uint8_t *payload, size_t payload_len) {
    
    uint32_t total_len = (uint32_t)(RPC_HEADER_SIZE + payload_len);
    uint32_t net_total_len = htonl(total_len);
    uint32_t net_request_id = htonl(request_id);

    memcpy(buffer, &net_total_len, 4);
    memcpy(buffer + 4, &net_request_id, 4);
    memcpy(buffer + 8, &node_id, sizeof(node_id_t));

    rpc_type_status_t type_and_status;
    type_and_status.fields.type = type;
    type_and_status.fields.status = status;
    buffer[10] = type_and_status.byte;

    buffer[11] = func_id;

    if (payload_len > 0 && payload != NULL) {
        memcpy(buffer + RPC_HEADER_SIZE, payload, payload_len);
    }

    return total_len;
}

int rpc_unpack_header(const uint8_t *buffer, rpc_header_t *header) {
    uint32_t net_total_len;
    uint32_t net_request_id;

    if (!buffer || !header) return -1;

    memcpy(&net_total_len, buffer, 4);
    memcpy(&net_request_id, buffer + 4, 4);

    header->total_len = ntohl(net_total_len);
    header->request_id = ntohl(net_request_id);
    
    // Extract sender_id (uint16_t at offset 8-9)
    memcpy(&header->sender_id, buffer + 8, sizeof(node_id_t));
    
    // Extract type_and_status at offset 10
    header->type_and_status.byte = buffer[10];
    
    // Extract func_id at offset 11
    header->func_id = buffer[11];

    if (header->total_len < RPC_HEADER_SIZE) return -1;

    return 0;
}
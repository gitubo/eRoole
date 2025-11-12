#include "roole/rpc/rpc_types.h"
#include "roole/core/common.h"

size_t rpc_pack_message(uint8_t *buffer, node_id_t node_id, uint32_t request_id,
                        uint8_t type, uint8_t status, uint8_t func_id,
                        const uint8_t *payload, size_t payload_len) {
    (void)buffer; (void)node_id; (void)request_id; (void)type;
    (void)status; (void)func_id; (void)payload; (void)payload_len;
    LOG_DEBUG("TODO: Implement rpc_pack_message");
    return 0;
}

int rpc_unpack_header(const uint8_t *buffer, rpc_header_t *header) {
    (void)buffer; (void)header;
    LOG_DEBUG("TODO: Implement rpc_unpack_header");
    return -1;
}
// include/roole/rpc/rpc_types.h
// RPC message types and constants

#ifndef ROOLE_RPC_TYPES_H
#define ROOLE_RPC_TYPES_H

#include "roole/core/common.h"
#include <stdint.h>

// RPC message header
#define RPC_HEADER_SIZE 12

// Message types
#define RPC_TYPE_STATUS 0x00
#define RPC_TYPE_REQUEST 0x01
#define RPC_TYPE_RESPONSE 0x02

// Status codes
typedef enum {
    RPC_STATUS_SUCCESS = 0x00,
    RPC_STATUS_BAD_ARGUMENT = 0x01,
    RPC_STATUS_FUNC_NOT_FOUND = 0x02,
    RPC_STATUS_INTERNAL_ERROR = 0x03,
    RPC_STATUS_NETWORK = 0x04,
    RPC_STATUS_TIMEOUT = 0x05,
    RPC_STATUS_UNKNOWN = 0xFF
} rpc_status_t;

// Function IDs
typedef enum {
    FUNC_ID_ADD = 0x01,  // Example
    
    // Cluster management
    FUNC_ID_JOIN_CLUSTER = 0x10,
    FUNC_ID_NODE_LIST = 0x11,
    FUNC_ID_HEARTBEAT = 0x12,
    FUNC_ID_GOSSIP = 0x13,
    FUNC_ID_LEAVE_CLUSTER = 0x14,
    
    // Router ↔ Worker
    FUNC_ID_EXECUTE_DAG = 0x20,
    FUNC_ID_WORKER_HEARTBEAT = 0x21,
    FUNC_ID_EXECUTION_UPDATE = 0x22,
    FUNC_ID_SYNC_CATALOG = 0x23,
    FUNC_ID_WORKER_REGISTRATION = 0x24,
    FUNC_ID_SUBMIT_MESSAGE = 0x25,
    FUNC_ID_PROCESS_MESSAGE = 0x26,
    
    // Datastore management
    FUNC_ID_DATASTORE_SET = 0x30,
    FUNC_ID_DATASTORE_GET = 0x31,
    FUNC_ID_DATASTORE_UNSET = 0x32,
    FUNC_ID_DATASTORE_LIST = 0x33,
    FUNC_ID_DATASTORE_SYNC = 0x34,
} rpc_func_id_t;

// RPC header structure
typedef union {
    uint8_t byte;
    struct {
        uint8_t status : 4;
        uint8_t type : 4;
    } fields;
} rpc_type_status_t;

typedef struct {
    uint32_t total_len;           // Total message length (header + payload)
    uint32_t request_id;          // Request ID
    node_id_t sender_id;          // Sender node ID
    rpc_type_status_t type_and_status;  // Combined type and status
    uint8_t func_id;              // Function ID
} rpc_header_t;

// Serialization functions
size_t rpc_pack_message(uint8_t *buffer, node_id_t node_id, uint32_t request_id,
                        uint8_t type, uint8_t status, uint8_t func_id,
                        const uint8_t *payload, size_t payload_len);

int rpc_unpack_header(const uint8_t *buffer, rpc_header_t *header);

#endif // ROOLE_RPC_TYPES_H
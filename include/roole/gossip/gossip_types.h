// include/roole/gossip/gossip_types.h
// Common gossip types (messages, enums, constants)

#ifndef ROOLE_GOSSIP_TYPES_H
#define ROOLE_GOSSIP_TYPES_H

#include "roole/core/common.h"
#include "roole/cluster/cluster_view.h"
#include <stdint.h>

// Message types (SWIM protocol)
typedef enum {
    GOSSIP_MSG_PING = 1,           // Direct health check
    GOSSIP_MSG_ACK = 2,            // Response to PING
    GOSSIP_MSG_PING_REQ = 3,       // Indirect ping (future)
    GOSSIP_MSG_SUSPECT = 4,        // Announce suspicion
    GOSSIP_MSG_ALIVE = 5,          // Refute suspicion
    GOSSIP_MSG_DEAD = 6,           // Declare node dead
    GOSSIP_MSG_JOIN = 7,           // New member joining
    GOSSIP_MSG_LEAVE = 8,          // Graceful leave
    GOSSIP_MSG_WORKER_JOIN = 9,    // Worker-specific join
    GOSSIP_MSG_JOIN_RESPONSE = 10  // Bootstrap response
} gossip_msg_type_t;

// Protocol configuration
typedef struct {
    uint32_t protocol_period_ms;   // How often to run SWIM round
    uint32_t ack_timeout_ms;       // PING → ACK timeout
    uint32_t suspect_timeout_ms;   // ALIVE → SUSPECT timeout (deprecated)
    uint32_t dead_timeout_ms;      // SUSPECT → DEAD timeout
    uint32_t fanout;               // Number of peers to gossip to
    uint32_t max_piggyback;        // Max updates per message
} gossip_config_t;

// Default configuration
static inline gossip_config_t gossip_default_config(void) {
    return (gossip_config_t){
        .protocol_period_ms = 1000,
        .ack_timeout_ms = 500,
        .suspect_timeout_ms = 5000,  // Unused
        .dead_timeout_ms = 5000,
        .fanout = 3,
        .max_piggyback = 10
    };
}

// Member update (piggybacked on messages)
#define MAX_IP_LEN 16
#define GOSSIP_MAX_PIGGYBACK_UPDATES 10

typedef struct {
    node_id_t node_id;
    node_type_t node_type;
    char ip_address[MAX_IP_LEN];
    uint16_t gossip_port;
    uint16_t data_port;
    node_status_t status;
    uint64_t incarnation;
    uint64_t timestamp_ms;
} gossip_member_update_t;

// Gossip message
#define GOSSIP_MAX_PAYLOAD_SIZE 1400

typedef struct {
    uint8_t version;               // Protocol version (currently 1)
    uint8_t msg_type;              // gossip_msg_type_t
    uint16_t flags;                // Reserved
    node_id_t sender_id;           // Who sent this
    node_type_t sender_type;     
    uint16_t sender_gossip_port; 
    uint16_t sender_data_port;   
    uint64_t sequence_num;         // Sender's sequence number
    uint8_t num_updates;           // Number of piggybacked updates
    gossip_member_update_t updates[GOSSIP_MAX_PIGGYBACK_UPDATES];
} gossip_message_t;

// Bootstrap response (list of routers)
#define MAX_CONFIG_STRING 256
#define MAX_CONFIG_ROUTERS 16

typedef struct {
    uint8_t num_routers;
    struct {
        node_id_t node_id;
        char gossip_addr[MAX_CONFIG_STRING];
        char data_addr[MAX_CONFIG_STRING];
    } routers[MAX_CONFIG_ROUTERS];
} gossip_bootstrap_response_t;

// Serialization functions
ssize_t gossip_message_serialize(const gossip_message_t *msg, uint8_t *buffer, size_t buffer_size);
int gossip_message_deserialize(const uint8_t *buffer, size_t buffer_size, gossip_message_t *msg);
ssize_t gossip_serialize_bootstrap_response(const gossip_bootstrap_response_t *resp, uint8_t *buffer, size_t buffer_size);
int gossip_deserialize_bootstrap_response(const uint8_t *buffer, size_t buffer_size, gossip_bootstrap_response_t *resp);
size_t gossip_message_serialized_size(const gossip_message_t *msg);

#endif // ROOLE_GOSSIP_TYPES_H
#ifndef ROOLE_CLUSTER_TYPES_H
#define ROOLE_CLUSTER_TYPES_H

#include "roole/core/common.h"
#include <stdint.h>

// Node types
typedef enum {
    NODE_TYPE_UNKNOWN = 0,
    NODE_TYPE_ROUTER = 1,
    NODE_TYPE_WORKER = 2
} node_type_t;

// Node status
typedef enum {
    NODE_STATUS_ALIVE = 0,
    NODE_STATUS_SUSPECT = 1,
    NODE_STATUS_DEAD = 2
} node_status_t;

// Cluster member
#define MAX_IP_LEN 16
#define MAX_CLUSTER_NODES 512

typedef struct {
    node_id_t node_id;
    node_type_t node_type;
    char ip_address[MAX_IP_LEN];
    uint16_t gossip_port;
    uint16_t data_port;
    node_status_t status;
    uint64_t last_seen_ms;
    uint64_t incarnation;
} cluster_member_t;

// Membership event callback (extracted from membership.h)
typedef void (*member_event_cb)(
    node_id_t node_id,
    node_type_t type,
    const char *ip,
    uint16_t data_port,
    const char *event_type,
    void *user_data
);

// Event types
#define MEMBER_EVENT_JOIN "member-join"
#define MEMBER_EVENT_LEAVE "member-leave"
#define MEMBER_EVENT_FAILED "member-failed"
#define MEMBER_EVENT_UPDATE "member-update"

#endif // ROOLE_CLUSTER_TYPES_H
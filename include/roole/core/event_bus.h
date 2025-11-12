// include/roole/event_bus.h
// (Unchanged from original)

#ifndef ROOLE_EVENT_BUS_H
#define ROOLE_EVENT_BUS_H

#include "roole/core/common.h"
#include "roole/cluster/cluster_view.h"
#include <stdint.h>
#include <errno.h>
#include <pthread.h>

// Event types
typedef enum {
    EVENT_TYPE_PEER_JOINED = 0,
    EVENT_TYPE_PEER_LEFT,
    EVENT_TYPE_PEER_FAILED,
    EVENT_TYPE_PEER_UPDATED,
    EVENT_TYPE_PEER_SUSPECT,
    EVENT_TYPE_EXECUTION_STARTED,
    EVENT_TYPE_EXECUTION_COMPLETED,
    EVENT_TYPE_EXECUTION_FAILED,
    EVENT_TYPE_MESSAGE_RECEIVED,
    EVENT_TYPE_MESSAGE_ROUTED,
    EVENT_TYPE_CATALOG_UPDATED,
    EVENT_TYPE_MAX
} event_type_t;

// Event data structures
typedef struct {
    node_id_t node_id;
    node_type_t node_type;
    char ip_address[MAX_IP_LEN];
    uint16_t gossip_port;
    uint16_t data_port;
    node_status_t status;
    uint64_t incarnation;
} event_peer_t;

typedef struct {
    execution_id_t exec_id;
    rule_id_t dag_id;
    node_id_t assigned_peer;
    uint64_t timestamp_ms;
    int status_code;
} event_execution_t;

typedef struct {
    execution_id_t exec_id;
    rule_id_t dag_id;
    node_id_t source_id;
    node_id_t dest_id;
    size_t message_size;
    uint64_t timestamp_ms;
} event_message_t;

typedef struct {
    rule_id_t dag_id;
    uint64_t version;
    char dag_name[64];
} event_catalog_t;

// Generic event
typedef struct {
    event_type_t type;
    uint64_t timestamp_ms;
    node_id_t source_node_id;
    
    union {
        event_peer_t peer;
        event_execution_t execution;
        event_message_t message;
        event_catalog_t catalog;
    } data;
} event_t;

// Event handler callback
typedef void (*event_handler_fn)(const event_t *event, void *user_data);

// Event bus
typedef struct event_bus event_bus_t;

// Lifecycle
event_bus_t* event_bus_create(void);
void event_bus_destroy(event_bus_t *bus);

// Subscription
int event_bus_subscribe(event_bus_t *bus, event_type_t type,
                        event_handler_fn handler, void *user_data);
int event_bus_unsubscribe(event_bus_t *bus, event_type_t type,
                          event_handler_fn handler);

// Publishing
int event_bus_publish(event_bus_t *bus, const event_t *event);
int event_bus_publish_sync(event_bus_t *bus, const event_t *event);

// Statistics
typedef struct {
    uint64_t events_published;
    uint64_t events_dispatched;
    uint64_t events_dropped;
    uint64_t queue_size;
    uint64_t subscribers_total;
} event_bus_stats_t;

void event_bus_get_stats(event_bus_t *bus, event_bus_stats_t *stats);

// Helpers
const char* event_type_to_string(event_type_t type);

#endif // ROOLE_EVENT_BUS_H
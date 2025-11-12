// include/roole/rpc/rpc_handler.h
// RPC handler registration and dispatch

#ifndef ROOLE_RPC_HANDLER_H
#define ROOLE_RPC_HANDLER_H

#include "roole/rpc/rpc_types.h"
#include <stddef.h>

typedef struct rpc_handler_registry rpc_handler_registry_t;

/**
 * Handler function signature
 * Synchronous handler: processes request, returns response
 * @param request Request payload
 * @param request_len Request length
 * @param response Output response pointer (handler allocates)
 * @param response_len Output response length
 * @param user_context User context (e.g., node_state_t*)
 * @return RPC status code (RPC_STATUS_SUCCESS, etc.)
 */
typedef int (*rpc_handler_fn)(
    const uint8_t *request,
    size_t request_len,
    uint8_t **response,
    size_t *response_len,
    void *user_context
);

/**
 * Create handler registry
 * @return Registry handle, or NULL on error
 */
rpc_handler_registry_t* rpc_handler_registry_create(void);

/**
 * Register handler for function ID
 * @param registry Registry handle
 * @param func_id Function ID
 * @param handler Handler function
 * @param user_context User context passed to handler
 * @return 0 on success, -1 on error
 */
int rpc_handler_register(
    rpc_handler_registry_t *registry,
    uint8_t func_id,
    rpc_handler_fn handler,
    void *user_context
);

/**
 * Unregister handler
 * @param registry Registry handle
 * @param func_id Function ID
 * @return 0 on success, -1 if not found
 */
int rpc_handler_unregister(rpc_handler_registry_t *registry, uint8_t func_id);

/**
 * Lookup handler by function ID
 * @param registry Registry handle
 * @param func_id Function ID
 * @param out_context Output user context
 * @return Handler function, or NULL if not found
 */
rpc_handler_fn rpc_handler_lookup(
    rpc_handler_registry_t *registry,
    uint8_t func_id,
    void **out_context
);

/**
 * Destroy handler registry
 * @param registry Registry handle
 */
void rpc_handler_registry_destroy(rpc_handler_registry_t *registry);

#endif // ROOLE_RPC_HANDLER_H
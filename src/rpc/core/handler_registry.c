#include "roole/rpc/rpc_handler.h"
#include "roole/core/common.h"

struct rpc_handler_registry {
    int dummy;
};

rpc_handler_registry_t* rpc_handler_registry_create(void) {
    LOG_INFO("TODO: Implement rpc_handler_registry_create");
    return NULL;
}

int rpc_handler_register(rpc_handler_registry_t *registry, uint8_t func_id,
                         rpc_handler_fn handler, void *user_context) {
    (void)registry; (void)func_id; (void)handler; (void)user_context;
    return -1;
}

int rpc_handler_unregister(rpc_handler_registry_t *registry, uint8_t func_id) {
    (void)registry; (void)func_id;
    return -1;
}

rpc_handler_fn rpc_handler_lookup(rpc_handler_registry_t *registry, uint8_t func_id, void **out_context) {
    (void)registry; (void)func_id; (void)out_context;
    return NULL;
}

void rpc_handler_registry_destroy(rpc_handler_registry_t *registry) {
    (void)registry;
}
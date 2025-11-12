#include "roole/rpc/rpc_server.h"
#include "roole/core/common.h"

struct rpc_server {
    int dummy;
};

rpc_server_t* rpc_server_create(const rpc_server_config_t *config,
                                rpc_handler_registry_t *handler_registry) {
    (void)config; (void)handler_registry;
    LOG_INFO("TODO: Implement rpc_server_create");
    return NULL;
}

int rpc_server_run(rpc_server_t *server) {
    (void)server;
    return -1;
}

void rpc_server_stop(rpc_server_t *server) {
    (void)server;
}

void rpc_server_destroy(rpc_server_t *server) {
    (void)server;
}

void rpc_server_get_stats(rpc_server_t *server, rpc_server_stats_t *out_stats) {
    (void)server;
    if (out_stats) memset(out_stats, 0, sizeof(rpc_server_stats_t));
}
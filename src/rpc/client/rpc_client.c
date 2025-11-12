#include "roole/rpc/rpc_client.h"
#include "roole/core/common.h"

struct rpc_client {
    int dummy;
};

rpc_client_t* rpc_client_connect(const char *remote_ip, uint16_t remote_port,
                                 rpc_channel_type_t channel_type, size_t buffer_size) {
    (void)remote_ip; (void)remote_port; (void)channel_type; (void)buffer_size;
    LOG_INFO("TODO: Implement rpc_client_connect");
    return NULL;
}

int rpc_client_call(rpc_client_t *client, uint8_t func_id,
                   const uint8_t *request, size_t request_len,
                   uint8_t **response, size_t *response_len, int timeout_ms) {
    (void)client; (void)func_id; (void)request; (void)request_len;
    (void)response; (void)response_len; (void)timeout_ms;
    return -1;
}

int rpc_client_send_async(rpc_client_t *client, uint8_t func_id,
                          const uint8_t *request, size_t request_len) {
    (void)client; (void)func_id; (void)request; (void)request_len;
    return -1;
}

void rpc_client_close(rpc_client_t *client) {
    (void)client;
}

rpc_channel_t* rpc_client_get_channel(rpc_client_t *client) {
    (void)client;
    return NULL;
}
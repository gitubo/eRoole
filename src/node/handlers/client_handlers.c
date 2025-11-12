#include "roole/node/node_handlers.h"
#include "roole/core/common.h"

int handle_submit_message(const uint8_t *request, size_t request_len,
                          uint8_t **response, size_t *response_len,
                          void *user_context) {
    (void)request; (void)request_len; (void)response; (void)response_len; (void)user_context;
    LOG_DEBUG("TODO: Implement handle_submit_message");
    return RPC_STATUS_INTERNAL_ERROR;
}

int handle_get_execution_status(const uint8_t *request, size_t request_len,
                                uint8_t **response, size_t *response_len,
                                void *user_context) {
    (void)request; (void)request_len; (void)response; (void)response_len; (void)user_context;
    return RPC_STATUS_INTERNAL_ERROR;
}

int handle_list_dags(const uint8_t *request, size_t request_len,
                    uint8_t **response, size_t *response_len,
                    void *user_context) {
    (void)request; (void)request_len; (void)response; (void)response_len; (void)user_context;
    return RPC_STATUS_INTERNAL_ERROR;
}

int handle_add_dag(const uint8_t *request, size_t request_len,
                  uint8_t **response, size_t *response_len,
                  void *user_context) {
    (void)request; (void)request_len; (void)response; (void)response_len; (void)user_context;
    return RPC_STATUS_INTERNAL_ERROR;
}
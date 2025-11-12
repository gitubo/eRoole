#include "roole/node/node_bootstrap.h"
#include "roole/core/common.h"

int node_bootstrap_from_config(node_state_t *state, const roole_config_t *config) {
    (void)state; (void)config;
    LOG_INFO("TODO: Implement node_bootstrap_from_config");
    return RESULT_ERR_INVALID;
}

int node_bootstrap_with_retry(node_state_t *state, const roole_config_t *config, int max_retries) {
    (void)state; (void)config; (void)max_retries;
    return RESULT_ERR_INVALID;
}
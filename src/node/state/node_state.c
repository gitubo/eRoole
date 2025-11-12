#include "roole/node/node_state.h"
#include "roole/core/common.h"

result_t node_state_init(node_state_t **state, const roole_config_t *config,
                         size_t num_executor_threads) {
    (void)config; (void)num_executor_threads;
    LOG_INFO("TODO: Implement node_state_init");
    *state = NULL;
    return RESULT_ERROR(RESULT_ERR_INVALID, "Not implemented");
}

result_t node_state_start(node_state_t *state) {
    (void)state;
    return RESULT_ERROR(RESULT_ERR_INVALID, "Not implemented");
}

result_t node_state_bootstrap(node_state_t *state, const roole_config_t *config) {
    (void)state; (void)config;
    return RESULT_ERROR(RESULT_ERR_INVALID, "Not implemented");
}

void node_state_shutdown(node_state_t *state) {
    (void)state;
}

void node_state_destroy(node_state_t *state) {
    (void)state;
}

const node_identity_t* node_state_get_identity(const node_state_t *state) {
    return state ? &state->identity : NULL;
}

const node_capabilities_t* node_state_get_capabilities(const node_state_t *state) {
    return state ? &state->capabilities : NULL;
}

dag_catalog_t* node_state_get_dag_catalog(node_state_t *state) {
    return state ? state->dag_catalog : NULL;
}

peer_pool_t* node_state_get_peer_pool(node_state_t *state) {
    return state ? state->peer_pool : NULL;
}

execution_tracker_t* node_state_get_exec_tracker(node_state_t *state) {
    return state ? state->exec_tracker : NULL;
}

message_queue_t* node_state_get_message_queue(node_state_t *state) {
    return state ? state->message_queue : NULL;
}

cluster_view_t* node_state_get_cluster_view(node_state_t *state) {
    return state ? state->cluster_view : NULL;
}

metrics_registry_t* node_state_get_metrics(node_state_t *state) {
    return state ? state->metrics_registry : NULL;
}

event_bus_t* node_state_get_event_bus(node_state_t *state) {
    return state ? state->event_bus : NULL;
}

void node_state_get_statistics(const node_state_t *state, node_statistics_t *stats) {
    (void)state;
    if (stats) memset(stats, 0, sizeof(node_statistics_t));
}
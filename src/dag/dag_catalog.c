// src/dag/dag_catalog.c

#include "roole/dag/dag.h"
#include "roole/core/common.h"
#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <endian.h>


// ============================================================================
// DAG CATALOG IMPLEMENTATION
// ============================================================================
// Add new function:
void dag_catalog_set_change_callback(dag_catalog_t *catalog,
                                     dag_catalog_change_cb callback,
                                     void *user_data) {
    if (!catalog) return;
    
    pthread_rwlock_wrlock(&catalog->lock);
    catalog->change_callback = callback;
    catalog->callback_user_data = user_data;
    pthread_rwlock_unlock(&catalog->lock);
}


int dag_catalog_init(dag_catalog_t *catalog, size_t capacity) {
    if (!catalog || capacity == 0) return RESULT_ERR_INVALID;
    
    memset(catalog, 0, sizeof(dag_catalog_t));
    
    catalog->dags = safe_calloc(capacity, sizeof(dag_t));
    if (!catalog->dags) {
        return RESULT_ERR_NOMEM;
    }
    
    catalog->capacity = capacity;
    catalog->count = 0;
    
    if (pthread_rwlock_init(&catalog->lock, NULL) != 0) {
        safe_free(catalog->dags);
        return RESULT_ERR_INVALID;
    }
    
    LOG_INFO("DAG catalog initialized (capacity: %zu)", capacity);

    return RESULT_OK;
}

void dag_catalog_destroy(dag_catalog_t *catalog) {
    if (!catalog) return;
    
    pthread_rwlock_wrlock(&catalog->lock);
    
    // Free all step config data
    for (size_t i = 0; i < catalog->count; i++) {
        for (size_t j = 0; j < catalog->dags[i].step_count; j++) {
            if (catalog->dags[i].steps[j].config_data) {
                safe_free(catalog->dags[i].steps[j].config_data);
            }
        }
    }
    
    safe_free(catalog->dags);
    catalog->dags = NULL;
    catalog->count = 0;
    catalog->capacity = 0;
    
    pthread_rwlock_unlock(&catalog->lock);
    pthread_rwlock_destroy(&catalog->lock);
    
    LOG_INFO("DAG catalog destroyed");
}

int dag_catalog_add(dag_catalog_t *catalog, const dag_t *dag) {
    if (!catalog || !dag) return RESULT_ERR_INVALID;
    
    pthread_rwlock_wrlock(&catalog->lock);
    
    // Check if DAG already exists
    for (size_t i = 0; i < catalog->count; i++) {
        if (catalog->dags[i].dag_id == dag->dag_id) {
            pthread_rwlock_unlock(&catalog->lock);
            LOG_WARN("DAG %u already exists", dag->dag_id);
            return RESULT_ERR_EXISTS;
        }
    }
    
    // Check capacity
    if (catalog->count >= catalog->capacity) {
        pthread_rwlock_unlock(&catalog->lock);
        LOG_ERROR("DAG catalog full (capacity: %zu)", catalog->capacity);
        return RESULT_ERR_FULL;
    }
    
    // Deep copy DAG
    dag_t *new_dag = &catalog->dags[catalog->count];
    memcpy(new_dag, dag, sizeof(dag_t));
    
    // Deep copy step config data
    for (size_t i = 0; i < new_dag->step_count; i++) {
        if (dag->steps[i].config_data && dag->steps[i].config_len > 0) {
            new_dag->steps[i].config_data = safe_malloc(dag->steps[i].config_len);
            if (!new_dag->steps[i].config_data) {
                // Cleanup on error
                for (size_t j = 0; j < i; j++) {
                    if (new_dag->steps[j].config_data) {
                        safe_free(new_dag->steps[j].config_data);
                    }
                }
                pthread_rwlock_unlock(&catalog->lock);
                return RESULT_ERR_NOMEM;
            }
            memcpy(new_dag->steps[i].config_data, dag->steps[i].config_data, 
                   dag->steps[i].config_len);
        }
    }
    
    catalog->count++;

    if (catalog->change_callback) {
        catalog->change_callback(catalog->count, catalog->callback_user_data);
    }
    
    pthread_rwlock_unlock(&catalog->lock);
    
    LOG_INFO("Added DAG %u '%s' (version %lu, %zu steps)", 
                   dag->dag_id, dag->name, dag->version, dag->step_count);
    return RESULT_OK;
}

int dag_catalog_update(dag_catalog_t *catalog, const dag_t *dag) {
    if (!catalog || !dag) return RESULT_ERR_INVALID;
    
    pthread_rwlock_wrlock(&catalog->lock);
    
    // Find existing DAG
    dag_t *existing = NULL;
    for (size_t i = 0; i < catalog->count; i++) {
        if (catalog->dags[i].dag_id == dag->dag_id) {
            existing = &catalog->dags[i];
            break;
        }
    }
    
    if (!existing) {
        pthread_rwlock_unlock(&catalog->lock);
        LOG_WARN("DAG %u not found for update", dag->dag_id);
        return RESULT_ERR_NOTFOUND;
    }
    
    // Free old config data
    for (size_t i = 0; i < existing->step_count; i++) {
        if (existing->steps[i].config_data) {
            safe_free(existing->steps[i].config_data);
        }
    }
    
    // Copy new DAG
    memcpy(existing, dag, sizeof(dag_t));
    
    // Deep copy new config data
    for (size_t i = 0; i < dag->step_count; i++) {
        if (dag->steps[i].config_data && dag->steps[i].config_len > 0) {
            existing->steps[i].config_data = safe_malloc(dag->steps[i].config_len);
            if (existing->steps[i].config_data) {
                memcpy(existing->steps[i].config_data, dag->steps[i].config_data, 
                       dag->steps[i].config_len);
            }
        }
    }
    
    existing->updated_at_ms = time_now_ms();
    
    pthread_rwlock_unlock(&catalog->lock);
    
    LOG_INFO("Updated DAG %u '%s' (version %lu)", 
                   dag->dag_id, dag->name, dag->version);
    return RESULT_OK;
}

int dag_catalog_remove(dag_catalog_t *catalog, rule_id_t dag_id) {
    if (!catalog) return RESULT_ERR_INVALID;
    
    pthread_rwlock_wrlock(&catalog->lock);
    
    // Find DAG
    size_t index = SIZE_MAX;
    for (size_t i = 0; i < catalog->count; i++) {
        if (catalog->dags[i].dag_id == dag_id) {
            index = i;
            break;
        }
    }
    
    if (index == SIZE_MAX) {
        pthread_rwlock_unlock(&catalog->lock);
        LOG_WARN("DAG %u not found for removal", dag_id);
        return RESULT_ERR_NOTFOUND;
    }
    
    // Free config data
    for (size_t i = 0; i < catalog->dags[index].step_count; i++) {
        if (catalog->dags[index].steps[i].config_data) {
            safe_free(catalog->dags[index].steps[i].config_data);
        }
    }
    
    // Shift remaining DAGs
    if (index < catalog->count - 1) {
        memmove(&catalog->dags[index], &catalog->dags[index + 1], 
                (catalog->count - index - 1) * sizeof(dag_t));
    }
    
    catalog->count--;

    if (catalog->change_callback) {
        catalog->change_callback(catalog->count, catalog->callback_user_data);
    }
    
    pthread_rwlock_unlock(&catalog->lock);
    
    LOG_INFO("Removed DAG %u", dag_id);
    return RESULT_OK;
}

dag_t* dag_catalog_get(dag_catalog_t *catalog, rule_id_t dag_id) {
    if (!catalog) return NULL;
    
    pthread_rwlock_rdlock(&catalog->lock);
    
    for (size_t i = 0; i < catalog->count; i++) {
        if (catalog->dags[i].dag_id == dag_id) {
            // Return pointer - caller must call dag_catalog_release when done
            return &catalog->dags[i];
        }
    }
    
    pthread_rwlock_unlock(&catalog->lock);
    return NULL;
}

void dag_catalog_release(dag_catalog_t *catalog) {
    if (catalog) {
        pthread_rwlock_unlock(&catalog->lock);
    }
}

size_t dag_catalog_list(dag_catalog_t *catalog, rule_id_t *out_dag_ids, size_t max_count) {
    if (!catalog || !out_dag_ids || max_count == 0) return 0;
    
    pthread_rwlock_rdlock(&catalog->lock);
    
    size_t count = ROOLE_MIN(catalog->count, max_count);
    for (size_t i = 0; i < count; i++) {
        out_dag_ids[i] = catalog->dags[i].dag_id;
    }
    
    pthread_rwlock_unlock(&catalog->lock);
    
    return count;
}


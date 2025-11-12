// include/roole/test/test_helpers.h
// Test utilities

#ifndef ROOLE_TEST_HELPERS_H
#define ROOLE_TEST_HELPERS_H

#include "roole/node/node_state.h"
#include "roole/cluster/cluster_view.h"
#include "roole/dag.h"

/**
 * Create minimal test node state
 * For unit testing handlers without full node initialization
 * @return Test node state (caller must destroy)
 */
node_state_t* test_create_node_state(void);

/**
 * Destroy test node state
 * @param state Test node state
 */
void test_destroy_node_state(node_state_t *state);

/**
 * Create test cluster view with sample members
 * @param num_members Number of members to add
 * @return Cluster view (caller must destroy)
 */
cluster_view_t* test_create_cluster_view(size_t num_members);

/**
 * Create test DAG
 * @param dag_id DAG ID
 * @param num_steps Number of steps
 * @return DAG (caller must free step configs and DAG itself)
 */
dag_t* test_create_dag(rule_id_t dag_id, size_t num_steps);

/**
 * Assert equals (aborts test on failure)
 */
#define TEST_ASSERT_EQ(expected, actual) \
    do { \
        if ((expected) != (actual)) { \
            fprintf(stderr, "ASSERT FAILED: %s:%d: expected %ld, got %ld\n", \
                    __FILE__, __LINE__, (long)(expected), (long)(actual)); \
            abort(); \
        } \
    } while(0)

/**
 * Assert not null
 */
#define TEST_ASSERT_NOT_NULL(ptr) \
    do { \
        if ((ptr) == NULL) { \
            fprintf(stderr, "ASSERT FAILED: %s:%d: pointer is NULL\n", \
                    __FILE__, __LINE__); \
            abort(); \
        } \
    } while(0)

/**
 * Assert null
 */
#define TEST_ASSERT_NULL(ptr) \
    do { \
        if ((ptr) != NULL) { \
            fprintf(stderr, "ASSERT FAILED: %s:%d: pointer is not NULL\n", \
                    __FILE__, __LINE__); \
            abort(); \
        } \
    } while(0)

#endif // ROOLE_TEST_HELPERS_H
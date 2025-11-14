// TODO: Move from dag_catalog.c
#include "roole/dag/dag.h"
#include "roole/core/common.h"
#include "roole/logger/logger.h"

// ============================================================================
// DAG VALIDATION
// ============================================================================

int dag_validate(const dag_t *dag) {
    if (!dag) return RESULT_ERR_INVALID;
    
    if (dag->step_count == 0 || dag->step_count > MAX_DAG_STEPS) {
        LOG_ERROR("Invalid step count: %zu", dag->step_count);
        return RESULT_ERR_INVALID;
    }
    
    // Check for valid step IDs and dependencies
    for (size_t i = 0; i < dag->step_count; i++) {
        const dag_step_t *step = &dag->steps[i];
        
        // Check dependencies exist
        for (size_t j = 0; j < step->dependency_count; j++) {
            uint32_t dep_id = step->dependencies[j];
            int found = 0;
            
            for (size_t k = 0; k < dag->step_count; k++) {
                if (dag->steps[k].step_id == dep_id) {
                    found = 1;
                    break;
                }
            }
            
            if (!found) {
                LOG_ERROR("Step %u has invalid dependency %u", 
                               step->step_id, dep_id);
                return RESULT_ERR_INVALID;
            }
        }
    }
    
    // TODO: Check for cycles using DFS
    
    return RESULT_OK;
}
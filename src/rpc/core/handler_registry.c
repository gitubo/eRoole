// src/rpc/core/handler_registry.c
// High-performance handler registry with thread-safe access

#include "roole/rpc/rpc_handler.h"
#include "roole/core/common.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#define MAX_HANDLERS 256

typedef struct handler_entry {
    uint8_t func_id;
    rpc_handler_fn handler;
    void *user_context;
    int active;  // Using int for atomic operations
} handler_entry_t;

struct rpc_handler_registry {
    handler_entry_t handlers[MAX_HANDLERS];
    pthread_rwlock_t lock;  // Read-write lock for scalability
    size_t count;
};

// ============================================================================
// Public API
// ============================================================================

rpc_handler_registry_t* rpc_handler_registry_create(void) {
    rpc_handler_registry_t *registry = (rpc_handler_registry_t*)safe_calloc(1, sizeof(rpc_handler_registry_t));
    if (!registry) {
        LOG_ERROR("Failed to allocate handler registry");
        return NULL;
    }
    
    if (pthread_rwlock_init(&registry->lock, NULL) != 0) {
        LOG_ERROR("Failed to initialize rwlock");
        safe_free(registry);
        return NULL;
    }
    
    registry->count = 0;
    
    LOG_DEBUG("Handler registry created");
    return registry;
}

int rpc_handler_register(rpc_handler_registry_t *registry, uint8_t func_id,
                         rpc_handler_fn handler, void *user_context) {
    if (!registry || !handler) {
        LOG_ERROR("Invalid arguments to register: registry=%p, handler=%p", 
                 (void*)registry, (void*)handler);
        return -1;
    }
    
    pthread_rwlock_wrlock(&registry->lock);
    
    // Check if already registered
    for (size_t i = 0; i < MAX_HANDLERS; i++) {
        if (registry->handlers[i].active && registry->handlers[i].func_id == func_id) {
            LOG_WARN("Handler for func_id=0x%02x already registered, updating", func_id);
            registry->handlers[i].handler = handler;
            registry->handlers[i].user_context = user_context;
            pthread_rwlock_unlock(&registry->lock);
            return 0;
        }
    }
    
    // Find free slot
    for (size_t i = 0; i < MAX_HANDLERS; i++) {
        if (!registry->handlers[i].active) {
            registry->handlers[i].func_id = func_id;
            registry->handlers[i].handler = handler;
            registry->handlers[i].user_context = user_context;
            registry->handlers[i].active = 1;
            registry->count++;
            
            pthread_rwlock_unlock(&registry->lock);
            LOG_INFO("Registered handler for func_id=0x%02x (total: %zu)", 
                    func_id, registry->count);
            return 0;
        }
    }
    
    pthread_rwlock_unlock(&registry->lock);
    LOG_ERROR("Handler registry full (max=%d)", MAX_HANDLERS);
    return -1;
}

int rpc_handler_unregister(rpc_handler_registry_t *registry, uint8_t func_id) {
    if (!registry) {
        LOG_ERROR("NULL registry");
        return -1;
    }
    
    pthread_rwlock_wrlock(&registry->lock);
    
    for (size_t i = 0; i < MAX_HANDLERS; i++) {
        if (registry->handlers[i].active && registry->handlers[i].func_id == func_id) {
            registry->handlers[i].active = 0;
            registry->handlers[i].handler = NULL;
            registry->handlers[i].user_context = NULL;
            registry->count--;
            
            pthread_rwlock_unlock(&registry->lock);
            LOG_INFO("Unregistered handler for func_id=0x%02x (remaining: %zu)", 
                    func_id, registry->count);
            return 0;
        }
    }
    
    pthread_rwlock_unlock(&registry->lock);
    LOG_WARN("Handler for func_id=0x%02x not found", func_id);
    return -1;
}

rpc_handler_fn rpc_handler_lookup(rpc_handler_registry_t *registry, 
                                  uint8_t func_id, void **out_context) {
    if (!registry) {
        LOG_ERROR("NULL registry");
        return NULL;
    }
    
    // Read lock for concurrent lookups
    pthread_rwlock_rdlock(&registry->lock);
    
    for (size_t i = 0; i < MAX_HANDLERS; i++) {
        if (registry->handlers[i].active && registry->handlers[i].func_id == func_id) {
            rpc_handler_fn handler = registry->handlers[i].handler;
            if (out_context) {
                *out_context = registry->handlers[i].user_context;
            }
            pthread_rwlock_unlock(&registry->lock);
            LOG_DEBUG("Handler lookup: func_id=0x%02x found", func_id);
            return handler;
        }
    }
    
    pthread_rwlock_unlock(&registry->lock);
    LOG_DEBUG("Handler lookup: func_id=0x%02x not found", func_id);
    return NULL;
}

void rpc_handler_registry_destroy(rpc_handler_registry_t *registry) {
    if (!registry) return;
    
    LOG_DEBUG("Destroying handler registry with %zu handlers", registry->count);
    
    pthread_rwlock_destroy(&registry->lock);
    safe_free(registry);
    
    LOG_DEBUG("Handler registry destroyed");
}
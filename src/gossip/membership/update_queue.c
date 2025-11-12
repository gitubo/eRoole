// src/gossip/membership/update_queue.c
// Circular buffer for gossip member updates

#define _POSIX_C_SOURCE 200809L

#include "roole/gossip/update_queue.h"
#include "roole/common.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#define MAX_UPDATE_QUEUE 256

struct update_queue {
    gossip_member_update_t updates[MAX_UPDATE_QUEUE];
    size_t head;
    size_t tail;
    size_t count;
    pthread_mutex_t lock;
};

update_queue_t* update_queue_create(void) {
    update_queue_t *queue = calloc(1, sizeof(update_queue_t));
    if (!queue) {
        LOG_ERROR("Failed to allocate update queue");
        return NULL;
    }
    
    if (pthread_mutex_init(&queue->lock, NULL) != 0) {
        LOG_ERROR("Failed to initialize update queue lock");
        free(queue);
        return NULL;
    }
    
    LOG_DEBUG("Update queue created (capacity: %d)", MAX_UPDATE_QUEUE);
    return queue;
}

void update_queue_destroy(update_queue_t *queue) {
    if (!queue) return;
    
    pthread_mutex_destroy(&queue->lock);
    free(queue);
    LOG_DEBUG("Update queue destroyed");
}

int update_queue_push(update_queue_t *queue, const gossip_member_update_t *update) {
    if (!queue || !update) return -1;
    
    pthread_mutex_lock(&queue->lock);
    
    if (queue->count >= MAX_UPDATE_QUEUE) {
        pthread_mutex_unlock(&queue->lock);
        LOG_WARN("Update queue full, dropping update for node %u", update->node_id);
        return -1;
    }
    
    queue->updates[queue->tail] = *update;
    queue->tail = (queue->tail + 1) % MAX_UPDATE_QUEUE;
    queue->count++;
    
    pthread_mutex_unlock(&queue->lock);
    
    LOG_DEBUG("Update queued for node %u (queue size: %zu)", update->node_id, queue->count);
    return 0;
}

int update_queue_pop_batch(update_queue_t *queue, gossip_member_update_t *out, 
                            size_t max_count) {
    if (!queue || !out || max_count == 0) return 0;
    
    pthread_mutex_lock(&queue->lock);
    
    size_t to_copy = (queue->count < max_count) ? queue->count : max_count;
    
    for (size_t i = 0; i < to_copy; i++) {
        out[i] = queue->updates[queue->head];
        queue->head = (queue->head + 1) % MAX_UPDATE_QUEUE;
        queue->count--;
    }
    
    pthread_mutex_unlock(&queue->lock);
    
    if (to_copy > 0) {
        LOG_DEBUG("Popped %zu updates from queue (remaining: %zu)", to_copy, queue->count);
    }
    
    return (int)to_copy;
}

size_t update_queue_size(update_queue_t *queue) {
    if (!queue) return 0;
    
    pthread_mutex_lock(&queue->lock);
    size_t size = queue->count;
    pthread_mutex_unlock(&queue->lock);
    
    return size;
}
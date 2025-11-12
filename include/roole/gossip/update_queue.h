// include/roole/gossip/update_queue.h
// Queue for buffering gossip updates before dissemination

#ifndef ROOLE_GOSSIP_UPDATE_QUEUE_H
#define ROOLE_GOSSIP_UPDATE_QUEUE_H

#include "roole/gossip/gossip_types.h"
#include <stddef.h>

// Opaque type
typedef struct update_queue update_queue_t;

/**
 * Create a new update queue
 * @return Queue handle or NULL on failure
 */
update_queue_t* update_queue_create(void);

/**
 * Destroy update queue
 */
void update_queue_destroy(update_queue_t *queue);

/**
 * Push an update to the queue
 * @return 0 on success, -1 if queue is full
 */
int update_queue_push(update_queue_t *queue, const gossip_member_update_t *update);

/**
 * Pop a batch of updates from the queue
 * @param out Output buffer for updates
 * @param max_count Maximum updates to retrieve
 * @return Number of updates actually retrieved
 */
int update_queue_pop_batch(update_queue_t *queue, gossip_member_update_t *out, 
                            size_t max_count);

/**
 * Get current queue size
 */
size_t update_queue_size(update_queue_t *queue);

#endif // ROOLE_GOSSIP_UPDATE_QUEUE_H
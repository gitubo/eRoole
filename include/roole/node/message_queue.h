// include/roole/node/message_queue.h
// Thread-safe message queue for executor threads

#ifndef ROOLE_NODE_MESSAGE_QUEUE_H
#define ROOLE_NODE_MESSAGE_QUEUE_H

#include "roole/core/common.h"
#include <pthread.h>

#define MAX_NODE_QUEUE_SIZE 1000
#define MAX_MESSAGE_SIZE 4096

// Message structure
typedef struct {
    execution_id_t exec_id;
    rule_id_t dag_id;
    uint8_t message_data[MAX_MESSAGE_SIZE];
    size_t message_len;
    uint64_t received_at_ms;
    node_id_t sender_id;
} message_t;

// Message queue (circular buffer)
typedef struct {
    message_t *messages;
    size_t head;
    size_t tail;
    size_t capacity;
    size_t count;
    pthread_mutex_t lock;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} message_queue_t;

/**
 * Initialize message queue
 * @param queue Queue structure
 * @param capacity Maximum messages
 * @return 0 on success, error code on failure
 */
int message_queue_init(message_queue_t *queue, size_t capacity);

/**
 * Destroy message queue
 * @param queue Queue structure
 */
void message_queue_destroy(message_queue_t *queue);

/**
 * Push message to queue (blocking if full)
 * @param queue Queue structure
 * @param message Message to push
 * @return 0 on success, error code on failure
 */
int message_queue_push(message_queue_t *queue, const message_t *message);

/**
 * Pop message from queue
 * @param queue Queue structure
 * @param out_message Output message
 * @param timeout_ms Timeout in milliseconds (-1 = block forever, 0 = non-blocking)
 * @return 0 on success, RESULT_ERR_TIMEOUT on timeout, RESULT_ERR_EMPTY if empty (non-blocking)
 */
int message_queue_pop(message_queue_t *queue, message_t *out_message, int timeout_ms);

/**
 * Get current queue size
 * @param queue Queue structure
 * @return Queue size
 */
size_t message_queue_size(message_queue_t *queue);

/**
 * Check if queue is empty
 * @param queue Queue structure
 * @return 1 if empty, 0 otherwise
 */
int message_queue_is_empty(message_queue_t *queue);

#endif // ROOLE_NODE_MESSAGE_QUEUE_H
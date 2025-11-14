/**
 * @file msg_queue.c
 * @brief Message queue implementation
 */

#include "protocol.h"
#include "../core/log.h"
#include <stdlib.h>
#include <string.h>

msg_queue_t* msg_queue_create(void) {
    msg_queue_t *queue = malloc(sizeof(msg_queue_t));
    if (!queue) {
        LOG_ERROR("Failed to allocate message queue");
        return NULL;
    }

    queue->head = NULL;
    queue->tail = NULL;
    queue->count = 0;

    pthread_mutex_init(&queue->lock, NULL);
    pthread_cond_init(&queue->not_empty, NULL);

    LOG_DEBUG("Message queue created");
    return queue;
}

void msg_queue_destroy(msg_queue_t *queue) {
    if (!queue) {
        return;
    }

    pthread_mutex_lock(&queue->lock);

    /* Free all entries */
    msg_queue_entry_t *curr = queue->head;
    while (curr) {
        msg_queue_entry_t *next = curr->next;
        free(curr);
        curr = next;
    }

    pthread_mutex_unlock(&queue->lock);
    pthread_mutex_destroy(&queue->lock);
    pthread_cond_destroy(&queue->not_empty);

    free(queue);
    LOG_DEBUG("Message queue destroyed");
}

int msg_queue_enqueue(msg_queue_t *queue, const message_t *msg, node_id_t dest) {
    if (!queue || !msg) {
        return DSM_ERROR_INVALID;
    }

    msg_queue_entry_t *entry = malloc(sizeof(msg_queue_entry_t));
    if (!entry) {
        LOG_ERROR("Failed to allocate queue entry");
        return DSM_ERROR_MEMORY;
    }

    memcpy(&entry->msg, msg, sizeof(message_t));
    entry->dest = dest;
    entry->next = NULL;

    pthread_mutex_lock(&queue->lock);

    if (queue->tail) {
        queue->tail->next = entry;
        queue->tail = entry;
    } else {
        queue->head = queue->tail = entry;
    }

    queue->count++;
    pthread_cond_signal(&queue->not_empty);

    pthread_mutex_unlock(&queue->lock);

    LOG_DEBUG("Message enqueued (type=%d, dest=%u, count=%d)",
              msg->header.type, dest, queue->count);
    return DSM_SUCCESS;
}

int msg_queue_dequeue(msg_queue_t *queue, message_t *msg, node_id_t *dest) {
    if (!queue || !msg || !dest) {
        return DSM_ERROR_INVALID;
    }

    pthread_mutex_lock(&queue->lock);

    /* Wait for messages */
    while (queue->count == 0) {
        pthread_cond_wait(&queue->not_empty, &queue->lock);
    }

    msg_queue_entry_t *entry = queue->head;
    queue->head = entry->next;
    if (!queue->head) {
        queue->tail = NULL;
    }

    queue->count--;

    memcpy(msg, &entry->msg, sizeof(message_t));
    *dest = entry->dest;

    pthread_mutex_unlock(&queue->lock);

    free(entry);

    LOG_DEBUG("Message dequeued (type=%d, dest=%u, count=%d)",
              msg->header.type, *dest, queue->count);
    return DSM_SUCCESS;
}

int msg_queue_size(msg_queue_t *queue) {
    if (!queue) {
        return 0;
    }

    pthread_mutex_lock(&queue->lock);
    int size = queue->count;
    pthread_mutex_unlock(&queue->lock);

    return size;
}

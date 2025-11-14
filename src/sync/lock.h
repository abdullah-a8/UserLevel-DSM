/**
 * @file lock.h
 * @brief Distributed lock internal structures
 */

#ifndef LOCK_H
#define LOCK_H

#include "dsm/types.h"
#include <pthread.h>

/**
 * Queue node for waiting requests
 */
typedef struct lock_waiter_s {
    node_id_t node_id;
    struct lock_waiter_s *next;
} lock_waiter_t;

/**
 * Distributed lock structure (opaque to users)
 */
struct dsm_lock_s {
    lock_id_t id;
    node_id_t holder;
    lock_state_t state;
    lock_waiter_t *waiters_head;
    lock_waiter_t *waiters_tail;
    pthread_mutex_t local_lock;
    pthread_cond_t acquired_cv;
};

#endif /* LOCK_H */

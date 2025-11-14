/**
 * @file barrier.h
 * @brief Distributed barrier internal structures
 */

#ifndef BARRIER_H
#define BARRIER_H

#include "dsm/types.h"
#include <pthread.h>

/**
 * Barrier state structure
 */
typedef struct {
    barrier_id_t id;
    int expected_count;
    int arrived_count;
    pthread_mutex_t lock;
    pthread_cond_t all_arrived_cv;
} dsm_barrier_t;

/**
 * Barrier manager (for centralized coordination)
 */
typedef struct {
    dsm_barrier_t *barriers;
    int max_barriers;
    pthread_mutex_t lock;
} barrier_manager_t;

#endif /* BARRIER_H */

/**
 * @file barrier.c
 * @brief Distributed barrier implementation (Day 9, Task 9.3-9.4)
 *
 * Implements centralized barrier synchronization using the manager node.
 * Each node sends BARRIER_ARRIVE to the manager, which broadcasts
 * BARRIER_RELEASE when all participants have arrived.
 */

#include "barrier.h"
#include "dsm/dsm.h"
#include "../core/dsm_context.h"
#include "../core/log.h"
#include "../network/handlers.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>

/* Timeout for barrier wait (30 seconds) */
#define BARRIER_TIMEOUT_SEC 30

/**
 * Find or create barrier by ID
 */
static dsm_barrier_t* find_or_create_barrier(barrier_id_t barrier_id, int num_participants) {
    dsm_context_t *ctx = dsm_get_context();

    pthread_mutex_lock(&ctx->barrier_mgr.lock);

    /* Search for existing barrier */
    for (int i = 0; i < ctx->barrier_mgr.max_barriers; i++) {
        dsm_barrier_t *b = &ctx->barrier_mgr.barriers[i];
        if (b->id == barrier_id && b->expected_count > 0) {
            pthread_mutex_unlock(&ctx->barrier_mgr.lock);
            return b;
        }
    }

    /* Find empty slot */
    for (int i = 0; i < ctx->barrier_mgr.max_barriers; i++) {
        dsm_barrier_t *b = &ctx->barrier_mgr.barriers[i];
        if (b->expected_count == 0) {
            /* Initialize barrier */
            b->id = barrier_id;
            b->expected_count = num_participants;
            b->arrived_count = 0;
            b->generation = 0;
            pthread_mutex_init(&b->lock, NULL);
            pthread_cond_init(&b->all_arrived_cv, NULL);

            pthread_mutex_unlock(&ctx->barrier_mgr.lock);
            LOG_DEBUG("Created barrier %lu (expecting %d participants)",
                      barrier_id, num_participants);
            return b;
        }
    }

    pthread_mutex_unlock(&ctx->barrier_mgr.lock);
    LOG_ERROR("Barrier manager full (max %d barriers)", ctx->barrier_mgr.max_barriers);
    return NULL;
}

/**
 * Distributed barrier synchronization
 *
 * Blocks until all participating nodes have reached the barrier.
 * All nodes must call with the same barrier_id and num_participants.
 */
int dsm_barrier(barrier_id_t barrier_id, int num_participants) {
    dsm_context_t *ctx = dsm_get_context();
    if (!ctx->initialized) {
        LOG_ERROR("DSM not initialized");
        return DSM_ERROR_INIT;
    }

    if (num_participants <= 0) {
        LOG_ERROR("Invalid num_participants: %d", num_participants);
        return DSM_ERROR_INVALID;
    }

    LOG_DEBUG("Node %u entering barrier %lu (expecting %d participants)",
              ctx->node_id, barrier_id, num_participants);

    dsm_barrier_t *barrier = find_or_create_barrier(barrier_id, num_participants);
    if (!barrier) {
        return DSM_ERROR_MEMORY;
    }

    /* If this is the manager node, handle locally */
    if (ctx->config.is_manager) {
        pthread_mutex_lock(&barrier->lock);

        int my_generation = barrier->generation;
        barrier->arrived_count++;

        LOG_DEBUG("Manager: barrier %lu arrived_count=%d/%d (gen=%d)",
                  barrier_id, barrier->arrived_count, barrier->expected_count, my_generation);

        if (barrier->arrived_count >= barrier->expected_count) {
            /* All participants arrived, release everyone */
            LOG_INFO("Manager: All %d participants arrived at barrier %lu, releasing",
                     barrier->arrived_count, barrier_id);

            /* CRITICAL FIX: Broadcast release to all nodes
             * Must iterate through ALL slots (MAX_NODES), not just num_nodes count,
             * because nodes are indexed by their node_id, not sequentially */
            for (int i = 0; i < MAX_NODES; i++) {
                if (ctx->network.nodes[i].connected && ctx->network.nodes[i].id != ctx->node_id) {
                    send_barrier_release(ctx->network.nodes[i].id, barrier_id);
                }
            }

            /* CRITICAL FIX: Increment generation BEFORE resetting count
             * This prevents race where fast nodes re-enter before others leave
             * Based on sense-reversal barrier best practices */
            barrier->generation++;

            /* Reset barrier for reuse */
            barrier->arrived_count = 0;

            /* Wake up local threads */
            pthread_cond_broadcast(&barrier->all_arrived_cv);
            pthread_mutex_unlock(&barrier->lock);
        } else {
            /* Wait for all to arrive (check for generation change) */
            struct timespec timeout;
            clock_gettime(CLOCK_REALTIME, &timeout);
            timeout.tv_sec += BARRIER_TIMEOUT_SEC;

            while (barrier->generation == my_generation) {
                int rc = pthread_cond_timedwait(&barrier->all_arrived_cv, &barrier->lock, &timeout);
                if (rc == ETIMEDOUT) {
                    pthread_mutex_unlock(&barrier->lock);
                    LOG_ERROR("Barrier %lu timeout (arrived: %d/%d, gen=%d)",
                              barrier_id, barrier->arrived_count, barrier->expected_count, my_generation);
                    return DSM_ERROR_TIMEOUT;
                }
            }

            pthread_mutex_unlock(&barrier->lock);
        }
    } else {
        /* Send BARRIER_ARRIVE to manager */
        pthread_mutex_lock(&barrier->lock);
        int my_generation = barrier->generation;
        pthread_mutex_unlock(&barrier->lock);

        node_id_t manager = 0;  /* Manager is always node 0 */
        int rc = send_barrier_arrive(manager, barrier_id, num_participants);
        if (rc != DSM_SUCCESS) {
            LOG_ERROR("Failed to send barrier arrive");
            return rc;
        }

        /* Wait for BARRIER_RELEASE from manager (generation change) */
        pthread_mutex_lock(&barrier->lock);

        struct timespec timeout;
        clock_gettime(CLOCK_REALTIME, &timeout);
        timeout.tv_sec += BARRIER_TIMEOUT_SEC;

        while (barrier->generation == my_generation) {
            rc = pthread_cond_timedwait(&barrier->all_arrived_cv, &barrier->lock, &timeout);
            if (rc == ETIMEDOUT) {
                pthread_mutex_unlock(&barrier->lock);
                LOG_ERROR("Barrier %lu timeout waiting for release (gen=%d)", barrier_id, my_generation);
                return DSM_ERROR_TIMEOUT;
            }
        }

        pthread_mutex_unlock(&barrier->lock);
    }

    /* Update statistics */
    pthread_mutex_lock(&ctx->stats_lock);
    ctx->stats.barrier_waits++;
    pthread_mutex_unlock(&ctx->stats_lock);

    LOG_DEBUG("Node %u passed barrier %lu", ctx->node_id, barrier_id);
    return DSM_SUCCESS;
}

/**
 * Manager-side: Handle barrier arrival from a node
 */
int barrier_manager_arrive(barrier_id_t barrier_id, node_id_t arriver, int num_participants) {
    dsm_context_t *ctx = dsm_get_context();

    dsm_barrier_t *barrier = find_or_create_barrier(barrier_id, num_participants);
    if (!barrier) {
        return DSM_ERROR_MEMORY;
    }

    pthread_mutex_lock(&barrier->lock);

    barrier->arrived_count++;
    LOG_DEBUG("Manager: Node %u arrived at barrier %lu (count=%d/%d)",
              arriver, barrier_id, barrier->arrived_count, barrier->expected_count);

    if (barrier->arrived_count >= barrier->expected_count) {
        /* All participants arrived, release everyone */
        LOG_INFO("Manager: All %d participants arrived at barrier %lu, releasing",
                 barrier->arrived_count, barrier_id);

        /* Broadcast release to all nodes (including the one that just arrived) */
        for (int i = 0; i < ctx->network.num_nodes; i++) {
            if (ctx->network.nodes[i].connected && ctx->network.nodes[i].id != ctx->node_id) {
                send_barrier_release(ctx->network.nodes[i].id, barrier_id);
            }
        }

        /* CRITICAL FIX: Increment generation BEFORE resetting count
         * This prevents race where fast nodes re-enter before others leave
         * Based on sense-reversal barrier best practices */
        barrier->generation++;

        /* Reset barrier for reuse */
        barrier->arrived_count = 0;

        /* Wake up any local threads waiting on this barrier */
        pthread_cond_broadcast(&barrier->all_arrived_cv);

        pthread_mutex_unlock(&barrier->lock);
    } else {
        pthread_mutex_unlock(&barrier->lock);
    }

    return DSM_SUCCESS;
}

/**
 * Client-side: Handle barrier release from manager
 */
int barrier_handle_release(barrier_id_t barrier_id) {
    dsm_context_t *ctx = dsm_get_context();

    /* Find the barrier */
    pthread_mutex_lock(&ctx->barrier_mgr.lock);

    dsm_barrier_t *barrier = NULL;
    for (int i = 0; i < ctx->barrier_mgr.max_barriers; i++) {
        dsm_barrier_t *b = &ctx->barrier_mgr.barriers[i];
        if (b->id == barrier_id && b->expected_count > 0) {
            barrier = b;
            break;
        }
    }

    pthread_mutex_unlock(&ctx->barrier_mgr.lock);

    if (!barrier) {
        LOG_WARN("Received barrier release for unknown barrier %lu", barrier_id);
        return DSM_ERROR_NOT_FOUND;
    }

    pthread_mutex_lock(&barrier->lock);

    /* Increment generation to signal release and wake up waiting threads */
    barrier->generation++;
    pthread_cond_broadcast(&barrier->all_arrived_cv);

    pthread_mutex_unlock(&barrier->lock);

    LOG_DEBUG("Node %u received barrier release for barrier %lu (gen=%d)",
              ctx->node_id, barrier_id, barrier->generation);
    return DSM_SUCCESS;
}

/**
 * @file lock.c
 * @brief Distributed lock implementation (Day 9, Task 9.1-9.2)
 *
 * Implements a centralized distributed lock using the manager node.
 * Lock requests are sent to the manager, which grants locks in FIFO order.
 */

#include "lock.h"
#include "dsm/dsm.h"
#include "../core/dsm_context.h"
#include "../core/log.h"
#include "../network/handlers.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>

/* Timeout for lock acquisition (5 seconds) */
#define LOCK_TIMEOUT_SEC 5

/**
 * Create a distributed lock
 *
 * Creates and registers a lock that can be acquired/released across nodes.
 * All nodes must create the same lock with the same ID.
 */
dsm_lock_t* dsm_lock_create(lock_id_t lock_id) {
    dsm_context_t *ctx = dsm_get_context();
    if (!ctx->initialized) {
        LOG_ERROR("DSM not initialized");
        return NULL;
    }

    LOG_DEBUG("Creating lock %lu", lock_id);

    /* Allocate lock structure */
    dsm_lock_t *lock = (dsm_lock_t*)calloc(1, sizeof(dsm_lock_t));
    if (!lock) {
        LOG_ERROR("Failed to allocate lock");
        return NULL;
    }

    /* Initialize lock fields */
    lock->id = lock_id;
    lock->holder = (node_id_t)-1;  /* -1 means no holder */
    lock->state = LOCK_STATE_FREE;
    lock->waiters_head = NULL;
    lock->waiters_tail = NULL;

    /* Initialize synchronization primitives */
    pthread_mutex_init(&lock->local_lock, NULL);
    pthread_cond_init(&lock->acquired_cv, NULL);

    /* Register lock with lock manager */
    pthread_mutex_lock(&ctx->lock_mgr.lock);

    /* Find empty slot */
    int slot = -1;
    for (int i = 0; i < 256; i++) {
        if (ctx->lock_mgr.locks[i] == NULL) {
            slot = i;
            break;
        }
        /* Check if lock with this ID already exists */
        if (ctx->lock_mgr.locks[i]->id == lock_id) {
            LOG_WARN("Lock %lu already exists", lock_id);
            pthread_mutex_unlock(&ctx->lock_mgr.lock);
            pthread_mutex_destroy(&lock->local_lock);
            pthread_cond_destroy(&lock->acquired_cv);
            free(lock);
            return ctx->lock_mgr.locks[i];
        }
    }

    if (slot == -1) {
        LOG_ERROR("Lock manager full (max 256 locks)");
        pthread_mutex_unlock(&ctx->lock_mgr.lock);
        pthread_mutex_destroy(&lock->local_lock);
        pthread_cond_destroy(&lock->acquired_cv);
        free(lock);
        return NULL;
    }

    ctx->lock_mgr.locks[slot] = lock;
    ctx->lock_mgr.num_locks++;
    pthread_mutex_unlock(&ctx->lock_mgr.lock);

    LOG_INFO("Created lock %lu (slot %d)", lock_id, slot);
    return lock;
}

/**
 * Find lock by ID in lock manager
 */
static dsm_lock_t* find_lock_by_id(lock_id_t lock_id) {
    dsm_context_t *ctx = dsm_get_context();

    pthread_mutex_lock(&ctx->lock_mgr.lock);
    dsm_lock_t *lock = NULL;
    for (int i = 0; i < 256; i++) {
        if (ctx->lock_mgr.locks[i] && ctx->lock_mgr.locks[i]->id == lock_id) {
            lock = ctx->lock_mgr.locks[i];
            break;
        }
    }
    pthread_mutex_unlock(&ctx->lock_mgr.lock);

    return lock;
}

/**
 * Acquire a distributed lock
 *
 * Blocks until the lock is acquired. Sends LOCK_REQUEST to manager node
 * and waits for LOCK_GRANT response.
 */
int dsm_lock_acquire(dsm_lock_t *lock) {
    if (!lock) {
        LOG_ERROR("NULL lock");
        return DSM_ERROR_INVALID;
    }

    dsm_context_t *ctx = dsm_get_context();
    if (!ctx->initialized) {
        LOG_ERROR("DSM not initialized");
        return DSM_ERROR_INIT;
    }

    LOG_DEBUG("Node %u acquiring lock %lu", ctx->node_id, lock->id);

    /* If this is the manager node, handle locally */
    if (ctx->config.is_manager) {
        pthread_mutex_lock(&lock->local_lock);

        /* Wait until lock is free */
        while (lock->state == LOCK_STATE_HELD) {
            struct timespec timeout;
            clock_gettime(CLOCK_REALTIME, &timeout);
            timeout.tv_sec += LOCK_TIMEOUT_SEC;

            int rc = pthread_cond_timedwait(&lock->acquired_cv, &lock->local_lock, &timeout);
            if (rc == ETIMEDOUT) {
                pthread_mutex_unlock(&lock->local_lock);
                LOG_ERROR("Lock acquire timeout for lock %lu", lock->id);
                return DSM_ERROR_TIMEOUT;
            }
        }

        /* Acquire the lock */
        lock->state = LOCK_STATE_HELD;
        lock->holder = ctx->node_id;
        pthread_mutex_unlock(&lock->local_lock);

        LOG_DEBUG("Node %u acquired lock %lu locally", ctx->node_id, lock->id);
    } else {
        /* Send LOCK_REQUEST to manager */
        node_id_t manager = 0;  /* Manager is always node 0 */
        int rc = send_lock_request(manager, lock->id);
        if (rc != DSM_SUCCESS) {
            LOG_ERROR("Failed to send lock request");
            return rc;
        }

        /* Wait for LOCK_GRANT */
        pthread_mutex_lock(&lock->local_lock);

        struct timespec timeout;
        clock_gettime(CLOCK_REALTIME, &timeout);
        timeout.tv_sec += LOCK_TIMEOUT_SEC;

        while (lock->holder != ctx->node_id) {
            rc = pthread_cond_timedwait(&lock->acquired_cv, &lock->local_lock, &timeout);
            if (rc == ETIMEDOUT) {
                pthread_mutex_unlock(&lock->local_lock);
                LOG_ERROR("Lock acquire timeout for lock %lu", lock->id);
                return DSM_ERROR_TIMEOUT;
            }
        }

        pthread_mutex_unlock(&lock->local_lock);
        LOG_DEBUG("Node %u acquired lock %lu from manager", ctx->node_id, lock->id);
    }

    /* Update statistics */
    pthread_mutex_lock(&ctx->stats_lock);
    ctx->stats.lock_acquires++;
    pthread_mutex_unlock(&ctx->stats_lock);

    return DSM_SUCCESS;
}

/**
 * Release a distributed lock
 *
 * Releases the lock, allowing another node to acquire it.
 * Must be called by the node that currently holds the lock.
 */
int dsm_lock_release(dsm_lock_t *lock) {
    if (!lock) {
        LOG_ERROR("NULL lock");
        return DSM_ERROR_INVALID;
    }

    dsm_context_t *ctx = dsm_get_context();
    if (!ctx->initialized) {
        LOG_ERROR("DSM not initialized");
        return DSM_ERROR_INIT;
    }

    LOG_DEBUG("Node %u releasing lock %lu", ctx->node_id, lock->id);

    /* If this is the manager node, handle locally */
    if (ctx->config.is_manager) {
        pthread_mutex_lock(&lock->local_lock);

        if (lock->holder != ctx->node_id) {
            pthread_mutex_unlock(&lock->local_lock);
            LOG_ERROR("Node %u doesn't hold lock %lu", ctx->node_id, lock->id);
            return DSM_ERROR_PERMISSION;
        }

        /* Release the lock */
        lock->state = LOCK_STATE_FREE;
        lock->holder = (node_id_t)-1;

        /* Wake up next waiter if any */
        pthread_cond_signal(&lock->acquired_cv);
        pthread_mutex_unlock(&lock->local_lock);

        LOG_DEBUG("Node %u released lock %lu locally", ctx->node_id, lock->id);
    } else {
        /* Send LOCK_RELEASE to manager */
        pthread_mutex_lock(&lock->local_lock);

        if (lock->holder != ctx->node_id) {
            pthread_mutex_unlock(&lock->local_lock);
            LOG_ERROR("Node %u doesn't hold lock %lu", ctx->node_id, lock->id);
            return DSM_ERROR_PERMISSION;
        }

        lock->holder = (node_id_t)-1;
        pthread_mutex_unlock(&lock->local_lock);

        node_id_t manager = 0;  /* Manager is always node 0 */
        int rc = send_lock_release(manager, lock->id);
        if (rc != DSM_SUCCESS) {
            LOG_ERROR("Failed to send lock release");
            return rc;
        }

        LOG_DEBUG("Node %u released lock %lu to manager", ctx->node_id, lock->id);
    }

    return DSM_SUCCESS;
}

/**
 * Destroy a distributed lock
 *
 * Cleans up lock resources. Should only be called after all nodes
 * are done using the lock.
 */
int dsm_lock_destroy(dsm_lock_t *lock) {
    if (!lock) {
        return DSM_ERROR_INVALID;
    }

    dsm_context_t *ctx = dsm_get_context();
    if (!ctx->initialized) {
        return DSM_ERROR_INIT;
    }

    LOG_DEBUG("Destroying lock %lu", lock->id);

    /* Remove from lock manager */
    pthread_mutex_lock(&ctx->lock_mgr.lock);
    for (int i = 0; i < 256; i++) {
        if (ctx->lock_mgr.locks[i] == lock) {
            ctx->lock_mgr.locks[i] = NULL;
            ctx->lock_mgr.num_locks--;
            break;
        }
    }
    pthread_mutex_unlock(&ctx->lock_mgr.lock);

    /* Cleanup waiters */
    lock_waiter_t *waiter = lock->waiters_head;
    while (waiter) {
        lock_waiter_t *next = waiter->next;
        free(waiter);
        waiter = next;
    }

    /* Cleanup synchronization primitives */
    pthread_mutex_destroy(&lock->local_lock);
    pthread_cond_destroy(&lock->acquired_cv);

    free(lock);
    return DSM_SUCCESS;
}

/**
 * Manager-side: Grant lock to a node (called by handler)
 */
int lock_manager_grant(lock_id_t lock_id, node_id_t requester) {
    dsm_lock_t *lock = find_lock_by_id(lock_id);
    if (!lock) {
        /* Lazy creation: create lock if it doesn't exist */
        LOG_DEBUG("Lock %lu not found on manager, creating lazily", lock_id);
        lock = dsm_lock_create(lock_id);
        if (!lock) {
            LOG_ERROR("Failed to create lock %lu", lock_id);
            return DSM_ERROR_MEMORY;
        }
    }

    dsm_context_t *ctx = dsm_get_context();

    pthread_mutex_lock(&lock->local_lock);

    if (lock->state == LOCK_STATE_FREE) {
        /* Lock is free, grant immediately */
        lock->state = LOCK_STATE_HELD;
        lock->holder = requester;

        /* Capture state for replication */
        node_id_t holder_copy = lock->holder;

        pthread_mutex_unlock(&lock->local_lock);

        /* Replicate lock state to backup */
        if (ctx->config.is_manager && !ctx->network.backup_state.is_promoted) {
            extern int send_state_sync_lock(lock_id_t lock_id, node_id_t holder, const node_id_t *waiters, int num_waiters);
            send_state_sync_lock(lock_id, holder_copy, NULL, 0);
        }

        LOG_DEBUG("Manager granted lock %lu to node %u", lock_id, requester);
        return send_lock_grant(requester, lock_id);
    } else {
        /* Lock is held, add to wait queue */
        lock_waiter_t *waiter = (lock_waiter_t*)malloc(sizeof(lock_waiter_t));
        if (!waiter) {
            pthread_mutex_unlock(&lock->local_lock);
            return DSM_ERROR_MEMORY;
        }

        waiter->node_id = requester;
        waiter->next = NULL;

        if (lock->waiters_tail) {
            lock->waiters_tail->next = waiter;
            lock->waiters_tail = waiter;
        } else {
            lock->waiters_head = lock->waiters_tail = waiter;
        }

        /* Capture waiter queue for replication */
        node_id_t holder_copy = lock->holder;
        node_id_t waiters_copy[32];
        int num_waiters = 0;
        lock_waiter_t *w = lock->waiters_head;
        while (w && num_waiters < 32) {
            waiters_copy[num_waiters++] = w->node_id;
            w = w->next;
        }

        pthread_mutex_unlock(&lock->local_lock);

        /* Replicate lock state to backup */
        if (ctx->config.is_manager && !ctx->network.backup_state.is_promoted) {
            extern int send_state_sync_lock(lock_id_t lock_id, node_id_t holder, const node_id_t *waiters, int num_waiters);
            send_state_sync_lock(lock_id, holder_copy, waiters_copy, num_waiters);
        }

        LOG_DEBUG("Manager queued node %u for lock %lu", requester, lock_id);
        return DSM_SUCCESS;
    }
}

/**
 * Manager-side: Handle lock release and grant to next waiter
 */
int lock_manager_release(lock_id_t lock_id, node_id_t releaser) {
    dsm_lock_t *lock = find_lock_by_id(lock_id);
    if (!lock) {
        /* This shouldn't happen, but log it */
        LOG_WARN("Lock %lu not found on release", lock_id);
        return DSM_ERROR_NOT_FOUND;
    }

    pthread_mutex_lock(&lock->local_lock);

    if (lock->holder != releaser) {
        pthread_mutex_unlock(&lock->local_lock);
        LOG_ERROR("Node %u doesn't hold lock %lu (holder is %u)",
                  releaser, lock_id, lock->holder);
        return DSM_ERROR_PERMISSION;
    }

    /* Check if there are waiters */
    if (lock->waiters_head) {
        /* Grant to next waiter */
        lock_waiter_t *next_waiter = lock->waiters_head;
        lock->waiters_head = next_waiter->next;
        if (!lock->waiters_head) {
            lock->waiters_tail = NULL;
        }

        node_id_t next_holder = next_waiter->node_id;
        free(next_waiter);

        lock->holder = next_holder;

        /* Capture remaining waiters for replication */
        node_id_t holder_copy = lock->holder;
        node_id_t waiters_copy[32];
        int num_waiters = 0;
        lock_waiter_t *w = lock->waiters_head;
        while (w && num_waiters < 32) {
            waiters_copy[num_waiters++] = w->node_id;
            w = w->next;
        }

        pthread_mutex_unlock(&lock->local_lock);

        /* Replicate lock state to backup */
        dsm_context_t *ctx = dsm_get_context();
        if (ctx->config.is_manager && !ctx->network.backup_state.is_promoted) {
            extern int send_state_sync_lock(lock_id_t lock_id, node_id_t holder, const node_id_t *waiters, int num_waiters);
            send_state_sync_lock(lock_id, holder_copy, waiters_copy, num_waiters);
        }

        LOG_DEBUG("Manager granted lock %lu to next waiter (node %u)", lock_id, next_holder);
        return send_lock_grant(next_holder, lock_id);
    } else {
        /* No waiters, mark as free */
        lock->state = LOCK_STATE_FREE;
        lock->holder = (node_id_t)-1;

        pthread_mutex_unlock(&lock->local_lock);

        /* Replicate lock state to backup */
        dsm_context_t *ctx = dsm_get_context();
        if (ctx->config.is_manager && !ctx->network.backup_state.is_promoted) {
            extern int send_state_sync_lock(lock_id_t lock_id, node_id_t holder, const node_id_t *waiters, int num_waiters);
            send_state_sync_lock(lock_id, (node_id_t)-1, NULL, 0);
        }

        LOG_DEBUG("Manager released lock %lu (no waiters)", lock_id);
        return DSM_SUCCESS;
    }
}

/**
 * Client-side: Handle lock grant from manager
 */
int lock_handle_grant(lock_id_t lock_id, node_id_t grantee) {
    dsm_lock_t *lock = find_lock_by_id(lock_id);
    if (!lock) {
        LOG_ERROR("Lock %lu not found", lock_id);
        return DSM_ERROR_NOT_FOUND;
    }

    dsm_context_t *ctx = dsm_get_context();
    if (grantee != ctx->node_id) {
        LOG_WARN("Received lock grant for node %u but I am node %u", grantee, ctx->node_id);
        return DSM_ERROR_INVALID;
    }

    pthread_mutex_lock(&lock->local_lock);
    lock->holder = grantee;
    pthread_cond_signal(&lock->acquired_cv);
    pthread_mutex_unlock(&lock->local_lock);

    LOG_DEBUG("Node %u received lock grant for lock %lu", grantee, lock_id);
    return DSM_SUCCESS;
}

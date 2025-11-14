/**
 * @file dsm.h
 * @brief Public API for User-Level Distributed Shared Memory
 *
 * This is the main header file that applications should include to use
 * the DSM system. It provides memory allocation, synchronization primitives,
 * and utility functions.
 */

#ifndef DSM_H
#define DSM_H

#include "types.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================ */
/*     Initialization           */
/* ============================ */

/**
 * Initialize the DSM system
 *
 * Must be called before any other DSM functions. This sets up:
 * - Page fault signal handlers
 * - Network connections
 * - Page table structures
 * - Lock and barrier managers
 *
 * @param config Configuration parameters
 * @return DSM_SUCCESS on success, error code on failure
 */
int dsm_init(const dsm_config_t *config);

/**
 * Finalize and cleanup the DSM system
 *
 * Shuts down network connections, frees resources, and restores
 * signal handlers. Should be called before program exit.
 *
 * @return DSM_SUCCESS on success, error code on failure
 */
int dsm_finalize(void);

/* ============================ */
/*     Memory Management        */
/* ============================ */

/**
 * Allocate DSM memory region
 *
 * Allocates a region of distributed shared memory that can be accessed
 * by all nodes in the cluster. The memory is initially invalid and will
 * be fault-in on first access.
 *
 * @param size Size in bytes (will be rounded up to page boundary)
 * @return Pointer to allocated memory, or NULL on failure
 */
void* dsm_malloc(size_t size);

/**
 * Free DSM memory region
 *
 * Deallocates a previously allocated DSM region.
 *
 * @param ptr Pointer returned by dsm_malloc
 * @return DSM_SUCCESS on success, error code on failure
 */
int dsm_free(void *ptr);

/**
 * Get current node ID
 *
 * @return This node's unique identifier
 */
node_id_t dsm_get_node_id(void);

/**
 * Get total number of nodes
 *
 * @return Total nodes in the DSM cluster
 */
int dsm_get_num_nodes(void);

/* ============================ */
/*     Synchronization          */
/* ============================ */

/**
 * Distributed lock handle (opaque)
 */
typedef struct dsm_lock_s dsm_lock_t;

/**
 * Create a distributed lock
 *
 * Creates a new distributed lock that can be acquired/released across nodes.
 * All nodes must create the same lock with the same ID.
 *
 * @param lock_id Unique lock identifier (must be same across nodes)
 * @return Lock handle, or NULL on failure
 */
dsm_lock_t* dsm_lock_create(lock_id_t lock_id);

/**
 * Acquire a distributed lock
 *
 * Blocks until the lock is acquired. Only one node can hold the lock
 * at any time.
 *
 * @param lock Lock handle from dsm_lock_create
 * @return DSM_SUCCESS on success, error code on failure
 */
int dsm_lock_acquire(dsm_lock_t *lock);

/**
 * Release a distributed lock
 *
 * Releases the lock, allowing another node to acquire it.
 * Must be called by the node that currently holds the lock.
 *
 * @param lock Lock handle
 * @return DSM_SUCCESS on success, error code on failure
 */
int dsm_lock_release(dsm_lock_t *lock);

/**
 * Destroy a distributed lock
 *
 * Cleans up lock resources. Should only be called after all nodes
 * are done using the lock.
 *
 * @param lock Lock handle
 * @return DSM_SUCCESS on success, error code on failure
 */
int dsm_lock_destroy(dsm_lock_t *lock);

/**
 * Distributed barrier synchronization
 *
 * Blocks until all participating nodes have reached the barrier.
 * All nodes must call with the same barrier_id and num_participants.
 *
 * @param barrier_id Unique barrier identifier
 * @param num_participants Number of nodes that must arrive
 * @return DSM_SUCCESS on success, error code on failure
 */
int dsm_barrier(barrier_id_t barrier_id, int num_participants);

/* ============================ */
/*     Statistics & Debugging   */
/* ============================ */

/**
 * Get DSM runtime statistics
 *
 * Returns counters for page faults, network transfers, etc.
 *
 * @param stats Pointer to stats structure to fill
 * @return DSM_SUCCESS on success, error code on failure
 */
int dsm_get_stats(dsm_stats_t *stats);

/**
 * Reset statistics counters
 *
 * Resets all counters to zero.
 *
 * @return DSM_SUCCESS on success, error code on failure
 */
int dsm_reset_stats(void);

/**
 * Print statistics to stdout
 *
 * Prints a formatted summary of current statistics.
 */
void dsm_print_stats(void);

/**
 * Set log level
 *
 * Controls verbosity of DSM logging output.
 *
 * @param level Log level (0=NONE, 1=ERROR, 2=WARN, 3=INFO, 4=DEBUG)
 */
void dsm_set_log_level(int level);

#ifdef __cplusplus
}
#endif

#endif /* DSM_H */

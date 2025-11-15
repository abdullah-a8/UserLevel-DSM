/**
 * @file dsm_context.h
 * @brief Global DSM state and context
 */

#ifndef DSM_CONTEXT_H
#define DSM_CONTEXT_H

#include "dsm/types.h"
#include "../memory/page_table.h"
#include "../network/protocol.h"
#include "../sync/lock.h"
#include "../sync/barrier.h"
#include <pthread.h>
#include <stdbool.h>

/**
 * Node information
 */
typedef struct {
    node_id_t id;
    char hostname[MAX_HOSTNAME_LEN];
    uint16_t port;
    int sockfd;
    bool connected;
} node_info_t;

/**
 * Network state
 */
typedef struct {
    int server_sockfd;
    uint16_t server_port;
    node_info_t nodes[MAX_NODES];
    int num_nodes;
    pthread_t dispatcher_thread;
    msg_queue_t *send_queue;
    bool running;
} network_state_t;

/**
 * Lock manager state
 */
typedef struct {
    struct dsm_lock_s *locks[256];
    int num_locks;
    pthread_mutex_t lock;
} lock_manager_t;

/**
 * Global DSM context (singleton)
 */
typedef struct {
    bool initialized;
    dsm_config_t config;
    node_id_t node_id;

    /* Memory management */
    page_table_t *page_table;  /* Primary page table (for backward compatibility) */
    page_table_t *page_tables[32];  /* Support up to 32 separate allocations */
    int num_allocations;

    /* Network */
    network_state_t network;

    /* Synchronization */
    lock_manager_t lock_mgr;
    barrier_manager_t barrier_mgr;

    /* Statistics */
    dsm_stats_t stats;
    pthread_mutex_t stats_lock;

    /* Global lock for context operations */
    pthread_mutex_t lock;
} dsm_context_t;

/**
 * Get global DSM context instance
 */
dsm_context_t* dsm_get_context(void);

/**
 * Initialize DSM context
 */
int dsm_context_init(const dsm_config_t *config);

/**
 * Cleanup DSM context
 */
void dsm_context_cleanup(void);

#endif /* DSM_CONTEXT_H */

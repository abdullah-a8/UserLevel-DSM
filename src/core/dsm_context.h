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

    /* Failure detection */
    uint64_t last_heartbeat_time;  /**< Timestamp of last heartbeat (nanoseconds) */
    int missed_heartbeats;         /**< Consecutive missed heartbeats */
    bool is_failed;                /**< True if node is considered failed */
} node_info_t;

/**
 * Allocation ACK tracking
 */
typedef struct {
    page_id_t start_page_id;       /**< Start of allocation being tracked */
    page_id_t end_page_id;         /**< End of allocation being tracked */
    int expected_acks;             /**< Number of ACKs expected */
    int received_acks;             /**< Number of ACKs received */
    bool acks_received[MAX_NODES]; /**< Track which nodes have ACK'd */
    pthread_mutex_t lock;          /**< Lock for this structure */
    pthread_cond_t all_acks_cv;    /**< Signaled when all ACKs received */
    bool active;                   /**< True if tracking an allocation */
} alloc_ack_tracker_t;

/**
 * Directory Query Tracker
 */
typedef struct {
    page_id_t page_id;           /**< Page being queried */
    node_id_t owner;             /**< Owner returned */
    bool active;                 /**< Is query active? */
    bool complete;               /**< Is query complete? */
    pthread_mutex_t lock;        /**< Lock */
    pthread_cond_t cv;           /**< Wait variable */
} dir_query_tracker_t;

/**
 * Network state
 */
typedef struct {
    int server_sockfd;
    uint16_t server_port;
    node_info_t nodes[MAX_NODES];
    int num_nodes;
    pthread_t dispatcher_thread;
    pthread_t heartbeat_thread;    /**< Heartbeat sender/checker thread */
    msg_queue_t *send_queue;
    bool running;

    /* Pending connections (not yet identified with NODE_JOIN) */
    int pending_sockets[MAX_NODES];
    int num_pending;
    pthread_mutex_t pending_lock;

    /* Sequence number for messages (for debugging and message tracking) */
    uint64_t next_seq_num;
    pthread_mutex_t seq_lock;

    /* Allocation ACK tracking */
    alloc_ack_tracker_t alloc_tracker;

    /* Directory Query tracking */
    dir_query_tracker_t dir_tracker;

    /* Sharer query tracker (for complete invalidation - BUG #8 fix) */
    struct {
        pthread_mutex_t lock;
        pthread_cond_t cv;
        page_id_t page_id;
        node_id_t sharers[32];  /* Use fixed size to avoid circular dependency */
        int num_sharers;
        bool active;
        bool complete;
    } sharer_tracker;
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
    int num_allocations;  /* Total allocations (local + remote) */
    int num_local_allocations;  /* CRITICAL: Track only LOCAL allocations for allocation_index calculation */
    pthread_mutex_t allocation_lock;  /* BUG FIX (BUG 3): Serialize allocations to prevent tracker corruption */

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

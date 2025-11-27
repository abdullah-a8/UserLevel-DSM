/**
 * @file protocol.h
 * @brief Network protocol message definitions
 *
 * This file defines all message types and structures used for
 * communication between DSM nodes.
 */

#ifndef PROTOCOL_H
#define PROTOCOL_H

#include "dsm/types.h"
#include <stdint.h>
#include <pthread.h>

/* Maximum number of sharers to track per page */
#ifndef MAX_SHARERS
#define MAX_SHARERS 32
#endif

/* ============================ */
/*     Message Types            */
/* ============================ */

/**
 * Network message types
 */
typedef enum {
    MSG_PAGE_REQUEST = 1,      /**< Request a page from owner */
    MSG_PAGE_REPLY,            /**< Reply with page data */
    MSG_INVALIDATE,            /**< Invalidate a page */
    MSG_INVALIDATE_ACK,        /**< Acknowledge invalidation */
    MSG_LOCK_REQUEST,          /**< Request lock acquisition */
    MSG_LOCK_GRANT,            /**< Grant lock to requester */
    MSG_LOCK_RELEASE,          /**< Release a lock */
    MSG_BARRIER_ARRIVE,        /**< Arrive at barrier */
    MSG_BARRIER_RELEASE,       /**< Release all from barrier */
    MSG_ALLOC_NOTIFY,          /**< Notify nodes of new allocation */
    MSG_ALLOC_ACK,             /**< Acknowledge allocation notification */
    MSG_NODE_JOIN,             /**< Node joining cluster */
    MSG_NODE_LEAVE,            /**< Node leaving cluster */
    MSG_HEARTBEAT,             /**< Keep-alive heartbeat */
    MSG_HEARTBEAT_ACK,         /**< Acknowledge heartbeat (optional) */
    MSG_DIR_QUERY,             /**< Query directory for page owner */
    MSG_DIR_REPLY,             /**< Reply with page owner */
    MSG_OWNER_UPDATE,          /**< Update directory with new owner */
    MSG_NODE_FAILED,           /**< Notify of node failure */
    MSG_SHARER_QUERY,          /**< Query owner for page sharers */
    MSG_SHARER_REPLY,          /**< Reply with page sharers list */
    MSG_ERROR,                 /**< Error response */
    /* Hot backup failover messages */
    MSG_STATE_SYNC_DIR,        /**< Replicate directory entry */
    MSG_STATE_SYNC_LOCK,       /**< Replicate lock state */
    MSG_STATE_SYNC_BARRIER,    /**< Replicate barrier state */
    MSG_STATE_SYNC_NODE,       /**< Replicate node metadata */
    MSG_MANAGER_PROMOTION,     /**< Backup announces promotion */
    MSG_RECONNECT_REQUEST      /**< Worker requests reconnection */
} msg_type_t;

/* ============================ */
/*     Message Header           */
/* ============================ */

/**
 * Common header for all messages
 *
 * Every message starts with this header, followed by type-specific payload.
 */
typedef struct {
    uint32_t magic;            /**< Magic number for validation (0xDEADBEEF) */
    msg_type_t type;           /**< Message type */
    uint32_t length;           /**< Payload length in bytes */
    node_id_t sender;          /**< Sender node ID */
    uint64_t seq_num;          /**< Sequence number */
} __attribute__((packed)) msg_header_t;

#define MSG_MAGIC 0xDEADBEEF

/* ============================ */
/*     Message Payloads         */
/* ============================ */

/**
 * PAGE_REQUEST message payload
 */
typedef struct {
    page_id_t page_id;         /**< Requested page ID */
    access_type_t access;      /**< READ or WRITE access */
    node_id_t requester;       /**< Requesting node ID */
} __attribute__((packed)) page_request_payload_t;

/**
 * PAGE_REPLY message payload
 */
typedef struct {
    page_id_t page_id;         /**< Page ID */
    uint64_t version;          /**< Page version number */
    access_type_t access;      /**< Access type granted (READ or WRITE) */
    node_id_t requester;       /**< Original requester (for manager proxying) */
    uint8_t data[PAGE_SIZE];   /**< Page data (4KB) */
} __attribute__((packed)) page_reply_payload_t;

/**
 * INVALIDATE message payload
 */
typedef struct {
    page_id_t page_id;         /**< Page to invalidate */
    node_id_t new_owner;       /**< New owner node ID */
    uint64_t version;          /**< New version number */
} __attribute__((packed)) invalidate_payload_t;

/**
 * INVALIDATE_ACK message payload
 */
typedef struct {
    page_id_t page_id;         /**< Acknowledged page ID */
    node_id_t acker;           /**< Node that acknowledged */
} __attribute__((packed)) invalidate_ack_payload_t;

/**
 * LOCK_REQUEST message payload
 */
typedef struct {
    lock_id_t lock_id;         /**< Lock identifier */
    node_id_t requester;       /**< Requesting node */
} __attribute__((packed)) lock_request_payload_t;

/**
 * LOCK_GRANT message payload
 */
typedef struct {
    lock_id_t lock_id;         /**< Lock identifier */
    node_id_t grantee;         /**< Node granted the lock */
} __attribute__((packed)) lock_grant_payload_t;

/**
 * LOCK_RELEASE message payload
 */
typedef struct {
    lock_id_t lock_id;         /**< Lock identifier */
    node_id_t releaser;        /**< Node releasing lock */
} __attribute__((packed)) lock_release_payload_t;

/**
 * BARRIER_ARRIVE message payload
 */
typedef struct {
    barrier_id_t barrier_id;   /**< Barrier identifier */
    node_id_t arriver;         /**< Arriving node */
    int num_participants;      /**< Total expected participants */
} __attribute__((packed)) barrier_arrive_payload_t;

/**
 * BARRIER_RELEASE message payload
 */
typedef struct {
    barrier_id_t barrier_id;   /**< Barrier identifier */
    int num_arrived;           /**< Number that arrived */
} __attribute__((packed)) barrier_release_payload_t;

/**
 * ALLOC_NOTIFY message payload
 * Sent by a node to notify all other nodes of a new allocation
 * Workers must create mmap at the same virtual address for SVAS
 */
typedef struct {
    page_id_t start_page_id;   /**< First page ID in allocation */
    page_id_t end_page_id;     /**< Last page ID in allocation (inclusive) */
    node_id_t owner;           /**< Owner node ID */
    size_t num_pages;          /**< Number of pages allocated */
    uint64_t base_addr;        /**< Virtual address of allocation (for SVAS) */
    size_t total_size;         /**< Total size in bytes */
} __attribute__((packed)) alloc_notify_payload_t;

/**
 * NODE_JOIN message payload
 */
typedef struct {
    node_id_t node_id;         /**< Joining node ID */
    char hostname[MAX_HOSTNAME_LEN]; /**< Hostname */
    uint16_t port;             /**< Port number */
} __attribute__((packed)) node_join_payload_t;

/**
 * NODE_LEAVE message payload
 */
typedef struct {
    node_id_t node_id;         /**< Leaving node ID */
} __attribute__((packed)) node_leave_payload_t;

/**
 * ALLOC_ACK message payload
 */
typedef struct {
    page_id_t start_page_id;   /**< First page ID in allocation */
    page_id_t end_page_id;     /**< Last page ID in allocation (inclusive) */
    node_id_t acker;           /**< Node that acknowledged */
} __attribute__((packed)) alloc_ack_payload_t;

/**
 * HEARTBEAT_ACK message payload (optional, can be empty)
 */
typedef struct {
    node_id_t acker;           /**< Node that acknowledged */
    uint64_t timestamp;        /**< Timestamp of ACK */
} __attribute__((packed)) heartbeat_ack_payload_t;

/**
 * DIR_QUERY message payload
 */
typedef struct {
    page_id_t page_id;         /**< Requested page ID */
    node_id_t requester;       /**< Requesting node ID */
} __attribute__((packed)) dir_query_payload_t;

/**
 * DIR_REPLY message payload
 */
typedef struct {
    page_id_t page_id;         /**< Page ID */
    node_id_t owner;           /**< Current owner node ID */
} __attribute__((packed)) dir_reply_payload_t;

/**
 * OWNER_UPDATE message payload
 */
typedef struct {
    page_id_t page_id;         /**< Page ID */
    node_id_t new_owner;       /**< New owner node ID */
} __attribute__((packed)) owner_update_payload_t;

/**
 * NODE_FAILED message payload
 */
typedef struct {
    node_id_t failed_node;     /**< Failed node ID */
} __attribute__((packed)) node_failed_payload_t;

/**
 * SHARER_QUERY message payload
 */
typedef struct {
    page_id_t page_id;         /**< Page ID */
    node_id_t requester;       /**< Requesting node ID */
} __attribute__((packed)) sharer_query_payload_t;

/**
 * SHARER_REPLY message payload
 */
typedef struct {
    page_id_t page_id;         /**< Page ID */
    int num_sharers;           /**< Number of sharers */
    node_id_t sharers[MAX_SHARERS]; /**< List of sharer node IDs */
} __attribute__((packed)) sharer_reply_payload_t;

/**
 * ERROR message payload
 */
typedef struct {
    int error_code;            /**< Error code */
    page_id_t page_id;         /**< Page ID related to error (if any) */
    char error_msg[256];       /**< Error description */
} __attribute__((packed)) error_payload_t;

/**
 * STATE_SYNC_DIR message payload
 * Replicates directory entry state to backup nodes
 */
typedef struct {
    uint64_t sync_seq;         /**< Sequence number for ordering */
    page_id_t page_id;         /**< Page ID */
    node_id_t owner;           /**< Current owner node ID */
    int num_sharers;           /**< Number of sharers */
    node_id_t sharers[MAX_SHARERS]; /**< List of sharer node IDs */
} __attribute__((packed)) state_sync_dir_payload_t;

/**
 * STATE_SYNC_LOCK message payload
 * Replicates lock state to backup nodes
 */
typedef struct {
    uint64_t sync_seq;         /**< Sequence number for ordering */
    lock_id_t lock_id;         /**< Lock identifier */
    node_id_t holder;          /**< Current lock holder (or -1 if free) */
    int num_waiters;           /**< Number of waiters in queue */
    node_id_t waiters[32];     /**< FIFO queue of waiters */
} __attribute__((packed)) state_sync_lock_payload_t;

/**
 * STATE_SYNC_BARRIER message payload
 * Replicates barrier state to backup nodes
 */
typedef struct {
    uint64_t sync_seq;         /**< Sequence number for ordering */
    barrier_id_t barrier_id;   /**< Barrier identifier */
    int num_arrived;           /**< Number of participants arrived */
    int num_expected;          /**< Total expected participants */
    uint64_t generation;       /**< Barrier generation number */
} __attribute__((packed)) state_sync_barrier_payload_t;

/**
 * STATE_SYNC_NODE message payload
 * Replicates node metadata to backup nodes
 */
typedef struct {
    uint64_t sync_seq;         /**< Sequence number for ordering */
    node_id_t node_id;         /**< Node ID */
    bool connected;            /**< Is node connected? */
    bool is_failed;            /**< Is node failed? */
} __attribute__((packed)) state_sync_node_payload_t;

/**
 * MANAGER_PROMOTION message payload
 * Broadcast by backup when it promotes to manager
 */
typedef struct {
    node_id_t new_manager_id;  /**< New manager node ID (promoted backup) */
    node_id_t old_manager_id;  /**< Old manager node ID (failed) */
    uint64_t promotion_time;   /**< Timestamp of promotion */
} __attribute__((packed)) manager_promotion_payload_t;

/**
 * RECONNECT_REQUEST message payload
 * Sent by workers to reconnect to new manager after failover
 */
typedef struct {
    node_id_t requester_id;    /**< Requesting node ID */
    uint64_t last_seq_seen;    /**< Last sequence number seen from old manager */
} __attribute__((packed)) reconnect_request_payload_t;

/* ============================ */
/*     Complete Message         */
/* ============================ */

/**
 * Complete network message structure
 *
 * Union of all possible message payloads.
 */
typedef struct {
    msg_header_t header;       /**< Message header */
    union {
        page_request_payload_t page_request;
        page_reply_payload_t page_reply;
        invalidate_payload_t invalidate;
        invalidate_ack_payload_t invalidate_ack;
        lock_request_payload_t lock_request;
        lock_grant_payload_t lock_grant;
        lock_release_payload_t lock_release;
        barrier_arrive_payload_t barrier_arrive;
        barrier_release_payload_t barrier_release;
        alloc_notify_payload_t alloc_notify;
        alloc_ack_payload_t alloc_ack;
        node_join_payload_t node_join;
        node_leave_payload_t node_leave;
        heartbeat_ack_payload_t heartbeat_ack;
        dir_query_payload_t dir_query;
        dir_reply_payload_t dir_reply;
        owner_update_payload_t owner_update;
        node_failed_payload_t node_failed;
        sharer_query_payload_t sharer_query;
        sharer_reply_payload_t sharer_reply;
        error_payload_t error;
        /* Hot backup failover payloads */
        state_sync_dir_payload_t state_sync_dir;
        state_sync_lock_payload_t state_sync_lock;
        state_sync_barrier_payload_t state_sync_barrier;
        state_sync_node_payload_t state_sync_node;
        manager_promotion_payload_t manager_promotion;
        reconnect_request_payload_t reconnect_request;
        uint8_t raw[PAGE_SIZE + 256]; /**< Raw buffer for largest payload */
    } payload;
} message_t;

/* ============================ */
/*     Message Queue Entry      */
/* ============================ */

/**
 * Entry in message queue for pending requests
 */
typedef struct msg_queue_entry_s {
    message_t msg;                    /**< The message */
    node_id_t dest;                   /**< Destination node */
    struct msg_queue_entry_s *next;   /**< Next in queue */
} msg_queue_entry_t;

/**
 * Message queue structure
 */
typedef struct {
    msg_queue_entry_t *head;   /**< Queue head */
    msg_queue_entry_t *tail;   /**< Queue tail */
    int count;                 /**< Number of messages */
    pthread_mutex_t lock;      /**< Queue lock */
    pthread_cond_t not_empty;  /**< Signaled when queue has messages */
} msg_queue_t;

/* ============================ */
/*     Function Declarations    */
/* ============================ */

/**
 * Create a message queue
 *
 * @return Pointer to queue, or NULL on failure
 */
msg_queue_t* msg_queue_create(void);

/**
 * Destroy a message queue
 *
 * @param queue Queue to destroy
 */
void msg_queue_destroy(msg_queue_t *queue);

/**
 * Enqueue a message
 *
 * @param queue Message queue
 * @param msg Message to enqueue
 * @param dest Destination node ID
 * @return DSM_SUCCESS on success, error code on failure
 */
int msg_queue_enqueue(msg_queue_t *queue, const message_t *msg, node_id_t dest);

/**
 * Dequeue a message (blocking)
 *
 * @param queue Message queue
 * @param msg Buffer to store dequeued message
 * @param dest Buffer to store destination node
 * @return DSM_SUCCESS on success, error code on failure
 */
int msg_queue_dequeue(msg_queue_t *queue, message_t *msg, node_id_t *dest);

/**
 * Get queue size
 *
 * @param queue Message queue
 * @return Number of messages in queue
 */
int msg_queue_size(msg_queue_t *queue);

#endif /* PROTOCOL_H */

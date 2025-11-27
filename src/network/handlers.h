/**
 * @file handlers.h
 * @brief Protocol message handlers
 */

#ifndef HANDLERS_H
#define HANDLERS_H

#include "protocol.h"

/* Page messages */
int send_page_request(node_id_t owner, page_id_t page_id, access_type_t access);
int send_page_reply(node_id_t requester, page_id_t page_id, access_type_t access, const void *data);
int handle_page_request(const message_t *msg);
int handle_page_reply(const message_t *msg);

/* Invalidate messages */
int send_invalidate(node_id_t target, page_id_t page_id);
int send_invalidate_ack(node_id_t target, page_id_t page_id);
int handle_invalidate(const message_t *msg);
int handle_invalidate_ack(const message_t *msg);

/* Lock messages */
int send_lock_request(node_id_t manager, lock_id_t lock_id);
int send_lock_grant(node_id_t grantee, lock_id_t lock_id);
int send_lock_release(node_id_t manager, lock_id_t lock_id);
int handle_lock_request(const message_t *msg);
int handle_lock_grant(const message_t *msg);
int handle_lock_release(const message_t *msg);

/* Barrier messages */
int send_barrier_arrive(node_id_t manager, barrier_id_t barrier_id, int num_participants);
int send_barrier_release(node_id_t node, barrier_id_t barrier_id);
int handle_barrier_arrive(const message_t *msg);
int handle_barrier_release(const message_t *msg);

/* Allocation notification messages */
int send_alloc_notify(page_id_t start_page_id, page_id_t end_page_id, node_id_t owner, size_t num_pages, void *base_addr, size_t total_size);
int send_alloc_ack(node_id_t target, page_id_t start_page_id, page_id_t end_page_id);
int handle_alloc_notify(const message_t *msg);
int handle_alloc_ack(const message_t *msg);

/* Allocation synchronization helpers */
int wait_for_alloc_acks(page_id_t start_page_id, page_id_t end_page_id, int expected_acks, int timeout_sec);

/* Node management messages */
int send_node_join(node_id_t node_id, const char *hostname, uint16_t port);
int handle_node_join(const message_t *msg, int sockfd);
int send_heartbeat(node_id_t target);
int handle_heartbeat(const message_t *msg);
int broadcast_node_failure(node_id_t failed_node);

/* Directory protocol messages */
int send_dir_query(node_id_t manager, page_id_t page_id);
int send_dir_reply(node_id_t requester, page_id_t page_id, node_id_t owner);
int send_owner_update(node_id_t manager, page_id_t page_id, node_id_t new_owner);
int handle_dir_query(const message_t *msg);
int handle_dir_reply(const message_t *msg);
int handle_owner_update(const message_t *msg);

/* Sharer tracking protocol (BUG #8 fix) */
int send_sharer_query(node_id_t owner, page_id_t page_id);
int handle_sharer_query(const message_t *msg);
int handle_sharer_reply(const message_t *msg);
int handle_node_failed_msg(const message_t *msg);

/* Failure detection */
void start_heartbeat_thread(void);
void stop_heartbeat_thread(void);

/* Hot backup failover - replication functions */
int send_state_sync_dir(page_id_t page_id, node_id_t owner, const node_id_t *sharers, int num_sharers);
int send_state_sync_lock(lock_id_t lock_id, node_id_t holder, const node_id_t *waiters, int num_waiters);
int send_state_sync_barrier(barrier_id_t barrier_id, int num_arrived, int num_expected, uint64_t generation);
int send_manager_promotion(node_id_t new_manager_id, node_id_t old_manager_id);
int send_reconnect_request(node_id_t new_manager);

/* Hot backup failover - handler functions */
int handle_state_sync_dir(const message_t *msg);
int handle_state_sync_lock(const message_t *msg);
int handle_state_sync_barrier(const message_t *msg);
int handle_manager_promotion(const message_t *msg);
int handle_reconnect_request(const message_t *msg);

/* Hot backup failover - promotion function */
int promote_to_manager(void);

/* Dispatch incoming message */
int dispatch_message(const message_t *msg, int sockfd);

#endif /* HANDLERS_H */

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

/* Failure detection */
void start_heartbeat_thread(void);
void stop_heartbeat_thread(void);

/* Dispatch incoming message */
int dispatch_message(const message_t *msg, int sockfd);

#endif /* HANDLERS_H */

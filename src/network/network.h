/**
 * @file network.h
 * @brief Network layer interface
 */

#ifndef NETWORK_H
#define NETWORK_H

#include "dsm/types.h"
#include "protocol.h"

/**
 * Initialize network server
 */
int network_server_init(uint16_t port);

/**
 * Connect to remote node
 */
int network_connect_to_node(node_id_t node_id, const char *hostname, uint16_t port);

/**
 * Send message to node
 */
int network_send(node_id_t dest, const message_t *msg);

/**
 * Receive message (blocking)
 */
int network_recv(int sockfd, message_t *msg);

/**
 * Start message dispatcher thread
 */
int network_start_dispatcher(void);

/**
 * Stop network subsystem
 */
void network_shutdown(void);

/**
 * Serialize message
 */
int serialize_message(const message_t *msg, uint8_t *buffer, size_t *len);

/**
 * Deserialize message
 */
int deserialize_message(const uint8_t *buffer, size_t len, message_t *msg);

#endif /* NETWORK_H */

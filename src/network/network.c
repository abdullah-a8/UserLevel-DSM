/**
 * @file network.c
 * @brief Network layer implementation
 */

#include "network.h"
#include "handlers.h"
#include "../core/log.h"
#include "../core/dsm_context.h"
#include "../core/perf_log.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <poll.h>

static void* accept_thread(void *arg);
static void* dispatcher_thread(void *arg);

int network_server_init(uint16_t port) {
    dsm_context_t *ctx = dsm_get_context();

    /* Create socket */
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        LOG_ERROR("Failed to create socket: %s", strerror(errno));
        return DSM_ERROR_NETWORK;
    }

    /* Set SO_REUSEADDR */
    int opt = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        LOG_ERROR("setsockopt SO_REUSEADDR failed: %s", strerror(errno));
        close(sockfd);
        return DSM_ERROR_NETWORK;
    }

    /* Bind */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("Bind failed: %s", strerror(errno));
        close(sockfd);
        return DSM_ERROR_NETWORK;
    }

    /* Listen */
    if (listen(sockfd, 10) < 0) {
        LOG_ERROR("Listen failed: %s", strerror(errno));
        close(sockfd);
        return DSM_ERROR_NETWORK;
    }

    ctx->network.server_sockfd = sockfd;
    ctx->network.server_port = port;
    ctx->network.running = true;

    /* Start accept thread */
    if (pthread_create(&ctx->network.dispatcher_thread, NULL, accept_thread, NULL) != 0) {
        LOG_ERROR("Failed to create accept thread");
        close(sockfd);
        return DSM_ERROR_INIT;
    }

    LOG_INFO("Network server listening on port %u", port);
    return DSM_SUCCESS;
}

static void* accept_thread(void *arg) {
    (void)arg;
    dsm_context_t *ctx = dsm_get_context();

    while (1) {
        /* Check if we should continue running and get server_sockfd (with lock) */
        pthread_mutex_lock(&ctx->lock);
        bool should_run = ctx->network.running;
        int server_fd = ctx->network.server_sockfd;
        pthread_mutex_unlock(&ctx->lock);

        if (!should_run || server_fd < 0) {
            break;
        }

        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);

        int client_fd = accept(server_fd,
                               (struct sockaddr*)&client_addr, &addr_len);
        if (client_fd < 0) {
            if (errno == EINTR) {
                break;
            }
            LOG_ERROR("Accept failed: %s", strerror(errno));
            continue;
        }

        LOG_INFO("Accepted connection from %s:%d (sockfd=%d)",
                 inet_ntoa(client_addr.sin_addr),
                 ntohs(client_addr.sin_port),
                 client_fd);

        /* Add to pending connections list - will be moved to nodes[] when NODE_JOIN is received */
        pthread_mutex_lock(&ctx->network.pending_lock);
        if (ctx->network.num_pending < MAX_NODES) {
            ctx->network.pending_sockets[ctx->network.num_pending++] = client_fd;
            LOG_DEBUG("Added sockfd=%d to pending connections (total pending=%d)",
                     client_fd, ctx->network.num_pending);
        } else {
            LOG_ERROR("Too many pending connections, rejecting sockfd=%d", client_fd);
            close(client_fd);
        }
        pthread_mutex_unlock(&ctx->network.pending_lock);
    }

    return NULL;
}

int network_connect_to_node(node_id_t node_id, const char *hostname, uint16_t port) {
    if (!hostname || node_id >= MAX_NODES) {
        return DSM_ERROR_INVALID;
    }

    dsm_context_t *ctx = dsm_get_context();

    /* Create socket */
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        LOG_ERROR("Socket creation failed: %s", strerror(errno));
        return DSM_ERROR_NETWORK;
    }

    /* Resolve hostname */
    struct hostent *host = gethostbyname(hostname);
    if (!host) {
        LOG_ERROR("Cannot resolve hostname: %s", hostname);
        close(sockfd);
        return DSM_ERROR_NETWORK;
    }

    /* Connect */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    memcpy(&addr.sin_addr, host->h_addr_list[0], host->h_length);
    addr.sin_port = htons(port);

    if (connect(sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("Connect to %s:%u failed: %s", hostname, port, strerror(errno));
        close(sockfd);
        return DSM_ERROR_NETWORK;
    }

    /* Store connection */
    pthread_mutex_lock(&ctx->lock);
    ctx->network.nodes[node_id].sockfd = sockfd;
    ctx->network.nodes[node_id].connected = true;
    ctx->network.nodes[node_id].id = node_id;
    ctx->network.nodes[node_id].port = port;
    strncpy(ctx->network.nodes[node_id].hostname, hostname, MAX_HOSTNAME_LEN - 1);

    /* CRITICAL FIX: Set running flag so heartbeat thread stays alive on worker nodes */
    ctx->network.running = true;

    pthread_mutex_unlock(&ctx->lock);

    LOG_INFO("Connected to node %u at %s:%u", node_id, hostname, port);
    return DSM_SUCCESS;
}

int serialize_message(const message_t *msg, uint8_t *buffer, size_t *len) {
    if (!msg || !buffer || !len) {
        return DSM_ERROR_INVALID;
    }

    /* Simple serialization: copy header + payload */
    size_t offset = 0;

    /* Header */
    memcpy(buffer + offset, &msg->header, sizeof(msg_header_t));
    offset += sizeof(msg_header_t);

    /* Payload based on type */
    switch (msg->header.type) {
        case MSG_PAGE_REQUEST:
            memcpy(buffer + offset, &msg->payload.page_request, sizeof(page_request_payload_t));
            offset += sizeof(page_request_payload_t);
            break;
        case MSG_PAGE_REPLY:
            memcpy(buffer + offset, &msg->payload.page_reply, sizeof(page_reply_payload_t));
            offset += sizeof(page_reply_payload_t);
            break;
        case MSG_INVALIDATE:
            memcpy(buffer + offset, &msg->payload.invalidate, sizeof(invalidate_payload_t));
            offset += sizeof(invalidate_payload_t);
            break;
        case MSG_INVALIDATE_ACK:
            memcpy(buffer + offset, &msg->payload.invalidate_ack, sizeof(invalidate_ack_payload_t));
            offset += sizeof(invalidate_ack_payload_t);
            break;
        case MSG_LOCK_REQUEST:
            memcpy(buffer + offset, &msg->payload.lock_request, sizeof(lock_request_payload_t));
            offset += sizeof(lock_request_payload_t);
            break;
        case MSG_LOCK_GRANT:
            memcpy(buffer + offset, &msg->payload.lock_grant, sizeof(lock_grant_payload_t));
            offset += sizeof(lock_grant_payload_t);
            break;
        case MSG_LOCK_RELEASE:
            memcpy(buffer + offset, &msg->payload.lock_release, sizeof(lock_release_payload_t));
            offset += sizeof(lock_release_payload_t);
            break;
        case MSG_BARRIER_ARRIVE:
            memcpy(buffer + offset, &msg->payload.barrier_arrive, sizeof(barrier_arrive_payload_t));
            offset += sizeof(barrier_arrive_payload_t);
            break;
        case MSG_BARRIER_RELEASE:
            memcpy(buffer + offset, &msg->payload.barrier_release, sizeof(barrier_release_payload_t));
            offset += sizeof(barrier_release_payload_t);
            break;
        case MSG_ALLOC_NOTIFY:
            memcpy(buffer + offset, &msg->payload.alloc_notify, sizeof(alloc_notify_payload_t));
            offset += sizeof(alloc_notify_payload_t);
            break;
        case MSG_ALLOC_ACK:
            memcpy(buffer + offset, &msg->payload.alloc_ack, sizeof(alloc_ack_payload_t));
            offset += sizeof(alloc_ack_payload_t);
            break;
        case MSG_NODE_JOIN:
            memcpy(buffer + offset, &msg->payload.node_join, sizeof(node_join_payload_t));
            offset += sizeof(node_join_payload_t);
            break;
        case MSG_NODE_LEAVE:
            memcpy(buffer + offset, &msg->payload.node_leave, sizeof(node_leave_payload_t));
            offset += sizeof(node_leave_payload_t);
            break;
        case MSG_HEARTBEAT:
            /* Heartbeat has no payload */
            break;
        case MSG_HEARTBEAT_ACK:
            memcpy(buffer + offset, &msg->payload.heartbeat_ack, sizeof(heartbeat_ack_payload_t));
            offset += sizeof(heartbeat_ack_payload_t);
            break;
        case MSG_ERROR:
            memcpy(buffer + offset, &msg->payload.error, sizeof(error_payload_t));
            offset += sizeof(error_payload_t);
            break;
        default:
            LOG_WARN("Unknown message type: %d", msg->header.type);
            return DSM_ERROR_INVALID;
    }

    *len = offset;
    return DSM_SUCCESS;
}

int deserialize_message(const uint8_t *buffer, size_t len, message_t *msg) {
    if (!buffer || !msg || len < sizeof(msg_header_t)) {
        return DSM_ERROR_INVALID;
    }

    size_t offset = 0;

    /* Header */
    memcpy(&msg->header, buffer + offset, sizeof(msg_header_t));
    offset += sizeof(msg_header_t);

    /* CRITICAL: Validate magic number to detect corruption
     * This prevents processing of corrupted or malformed messages */
    if (msg->header.magic != MSG_MAGIC) {
        LOG_ERROR("Invalid magic number: expected 0x%X, got 0x%X",
                  MSG_MAGIC, msg->header.magic);
        return DSM_ERROR_INVALID;
    }

    /* Validate message type */
    if (msg->header.type < 1 || msg->header.type > MSG_ERROR) {
        LOG_ERROR("Invalid message type: %d", msg->header.type);
        return DSM_ERROR_INVALID;
    }

    /* Payload */
    size_t remaining = len - offset;
    if (remaining > 0) {
        memcpy(&msg->payload.raw, buffer + offset, remaining);
    }

    return DSM_SUCCESS;
}

int network_send(node_id_t dest, message_t *msg) {
    if (!msg || dest >= MAX_NODES) {
        return DSM_ERROR_INVALID;
    }

    dsm_context_t *ctx = dsm_get_context();

    /* Assign sequence number atomically for message tracking and debugging */
    pthread_mutex_lock(&ctx->network.seq_lock);
    msg->header.seq_num = ctx->network.next_seq_num++;
    pthread_mutex_unlock(&ctx->network.seq_lock);

    pthread_mutex_lock(&ctx->lock);
    if (!ctx->network.nodes[dest].connected) {
        pthread_mutex_unlock(&ctx->lock);
        LOG_ERROR("Node %u not connected", dest);
        return DSM_ERROR_NETWORK;
    }

    /* PERFORMANCE FIX: Check if node has failed - avoid sending to dead nodes */
    if (ctx->network.nodes[dest].is_failed) {
        pthread_mutex_unlock(&ctx->lock);
        LOG_WARN("Skipping send to failed node %u (detected via heartbeat)", dest);
        return DSM_ERROR_NETWORK;
    }

    int sockfd = ctx->network.nodes[dest].sockfd;
    pthread_mutex_unlock(&ctx->lock);

    /* Serialize message to get payload */
    uint8_t msg_buffer[8192];
    size_t msg_len;
    if (serialize_message(msg, msg_buffer, &msg_len) != DSM_SUCCESS) {
        return DSM_ERROR_INVALID;
    }

    /* CRITICAL FIX: Add length prefix for proper TCP message framing
     * This prevents message boundary loss when multiple messages arrive together */
    uint8_t buffer[8196];  /* 4 bytes for length + 8192 for message */
    uint32_t length_prefix = (uint32_t)msg_len;

    /* Write length prefix (network byte order) */
    buffer[0] = (length_prefix >> 24) & 0xFF;
    buffer[1] = (length_prefix >> 16) & 0xFF;
    buffer[2] = (length_prefix >> 8) & 0xFF;
    buffer[3] = length_prefix & 0xFF;

    /* Copy message after length prefix */
    memcpy(buffer + 4, msg_buffer, msg_len);
    size_t len = msg_len + 4;  /* Total: 4-byte prefix + message */

    /* Task 8.4: Network failure handling with retries */
    const int MAX_RETRIES = 3;
    const int RETRY_DELAY_MS = 100;  /* 100ms delay between retries */
    int retry_count = 0;

    while (retry_count < MAX_RETRIES) {
        /* Send with full data transmission */
        size_t total_sent = 0;
        while (total_sent < len) {
            ssize_t sent = send(sockfd, buffer + total_sent, len - total_sent, MSG_NOSIGNAL);

            if (sent < 0) {
                /* Handle different error types */
                if (errno == EINTR) {
                    /* Interrupted by signal, retry immediately */
                    continue;
                } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    /* Would block, wait and retry */
                    struct timespec wait = {0, RETRY_DELAY_MS * 1000000};
                    nanosleep(&wait, NULL);
                    continue;
                } else if (errno == EPIPE || errno == ECONNRESET) {
                    /* Connection broken, mark as disconnected */
                    LOG_ERROR("Connection to node %u broken: %s", dest, strerror(errno));
                    pthread_mutex_lock(&ctx->lock);
                    ctx->network.nodes[dest].connected = false;
                    pthread_mutex_unlock(&ctx->lock);
                    return DSM_ERROR_NETWORK;
                } else {
                    /* Other error, retry */
                    LOG_WARN("Send to node %u failed (attempt %d/%d): %s",
                             dest, retry_count + 1, MAX_RETRIES, strerror(errno));
                    break;  /* Exit inner loop to retry */
                }
            } else if (sent == 0) {
                /* Connection closed */
                LOG_ERROR("Connection to node %u closed", dest);
                pthread_mutex_lock(&ctx->lock);
                ctx->network.nodes[dest].connected = false;
                pthread_mutex_unlock(&ctx->lock);
                return DSM_ERROR_NETWORK;
            } else {
                /* Successful send */
                total_sent += sent;
            }
        }

        /* Check if we sent all data successfully */
        if (total_sent == len) {
            LOG_DEBUG("Sent message type=%d to node %u (%zu bytes)", msg->header.type, dest, len);
            return DSM_SUCCESS;
        }

        /* Retry after delay */
        retry_count++;
        if (retry_count < MAX_RETRIES) {
            /* Track retry in performance stats (Task 8.6) */
            perf_log_network_retry();
            struct timespec wait = {0, RETRY_DELAY_MS * 1000000 * retry_count};
            nanosleep(&wait, NULL);
        }
    }

    /* All retries exhausted (Task 8.6) */
    LOG_ERROR("Failed to send message to node %u after %d retries", dest, MAX_RETRIES);
    perf_log_network_failure();
    return DSM_ERROR_NETWORK;
}

int network_recv(int sockfd, message_t *msg) {
    if (sockfd < 0 || !msg) {
        return DSM_ERROR_INVALID;
    }

    /* CRITICAL FIX: Read length prefix first (4 bytes, network byte order)
     * This ensures we read exactly one complete message, handling TCP streaming correctly */
    uint8_t length_buf[4];
    size_t total_read = 0;

    /* Read exactly 4 bytes for length prefix */
    while (total_read < 4) {
        ssize_t n = recv(sockfd, length_buf + total_read, 4 - total_read, 0);
        if (n <= 0) {
            if (n == 0) {
                LOG_INFO("Connection closed");
            } else {
                LOG_ERROR("Recv failed reading length: %s", strerror(errno));
            }
            return DSM_ERROR_NETWORK;
        }
        total_read += n;
    }

    /* Decode length (network byte order to host) */
    uint32_t msg_len = ((uint32_t)length_buf[0] << 24) |
                       ((uint32_t)length_buf[1] << 16) |
                       ((uint32_t)length_buf[2] << 8) |
                       ((uint32_t)length_buf[3]);

    /* Validate message length */
    if (msg_len == 0 || msg_len > 8192) {
        LOG_ERROR("Invalid message length: %u", msg_len);
        return DSM_ERROR_INVALID;
    }

    /* Read exactly msg_len bytes for the message payload */
    uint8_t buffer[8192];
    total_read = 0;

    while (total_read < msg_len) {
        ssize_t n = recv(sockfd, buffer + total_read, msg_len - total_read, 0);
        if (n <= 0) {
            if (n == 0) {
                LOG_INFO("Connection closed while reading message");
            } else {
                LOG_ERROR("Recv failed reading message: %s", strerror(errno));
            }
            return DSM_ERROR_NETWORK;
        }
        total_read += n;
    }

    return deserialize_message(buffer, msg_len, msg);
}

int network_start_dispatcher(void) {
    dsm_context_t *ctx = dsm_get_context();

    if (pthread_create(&ctx->network.dispatcher_thread, NULL, dispatcher_thread, NULL) != 0) {
        LOG_ERROR("Failed to create dispatcher thread");
        return DSM_ERROR_INIT;
    }

    LOG_INFO("Network dispatcher started");
    return DSM_SUCCESS;
}

static void* dispatcher_thread(void *arg) {
    (void)arg;
    dsm_context_t *ctx = dsm_get_context();

    while (ctx->network.running) {
        struct pollfd fds[MAX_NODES * 2];  /* Connected + pending */
        int nfds = 0;

        /* Add connected nodes */
        pthread_mutex_lock(&ctx->lock);
        for (int i = 0; i < MAX_NODES; i++) {
            if (ctx->network.nodes[i].connected) {
                fds[nfds].fd = ctx->network.nodes[i].sockfd;
                fds[nfds].events = POLLIN;
                nfds++;
            }
        }
        pthread_mutex_unlock(&ctx->lock);

        /* Add pending connections (waiting for NODE_JOIN) */
        pthread_mutex_lock(&ctx->network.pending_lock);
        for (int i = 0; i < ctx->network.num_pending; i++) {
            fds[nfds].fd = ctx->network.pending_sockets[i];
            fds[nfds].events = POLLIN;
            nfds++;
        }
        pthread_mutex_unlock(&ctx->network.pending_lock);

        if (nfds == 0) {
            usleep(100000);
            continue;
        }

        int ret = poll(fds, nfds, 1000);
        if (ret < 0) {
            if (errno == EINTR) continue;
            LOG_ERROR("Poll failed: %s", strerror(errno));
            break;
        }

        if (ret == 0) continue;

        for (int i = 0; i < nfds; i++) {
            if (fds[i].revents & POLLIN) {
                message_t msg;
                int sockfd = fds[i].fd;
                if (network_recv(sockfd, &msg) == DSM_SUCCESS) {
                    /* Enhanced logging to track ALLOC_NOTIFY messages */
                    if (msg.header.type == MSG_ALLOC_NOTIFY) {
                        LOG_INFO("DISPATCHER: Received ALLOC_NOTIFY from sender=%u (sockfd=%d, pages=%lu-%lu)",
                                 msg.header.sender, sockfd,
                                 msg.payload.alloc_notify.start_page_id,
                                 msg.payload.alloc_notify.end_page_id);
                    } else {
                        LOG_DEBUG("Received message type=%d from sockfd=%d", msg.header.type, sockfd);
                    }
                    dispatch_message(&msg, sockfd);
                }
            }
        }
    }

    return NULL;
}

void network_shutdown(void) {
    dsm_context_t *ctx = dsm_get_context();

    /* Set running flag and close server socket with lock */
    pthread_mutex_lock(&ctx->lock);
    bool was_running = ctx->network.running;
    ctx->network.running = false;
    pthread_mutex_unlock(&ctx->lock);

    /* CRITICAL FIX #2: Stop heartbeat thread before closing connections */
    extern void stop_heartbeat_thread(void);
    stop_heartbeat_thread();

    pthread_mutex_lock(&ctx->lock);
    int server_fd = ctx->network.server_sockfd;
    ctx->network.server_sockfd = -1;
    pthread_t thread = ctx->network.dispatcher_thread;
    ctx->network.dispatcher_thread = 0;  /* Reset thread handle */
    pthread_mutex_unlock(&ctx->lock);

    /* Close server socket to wake up accept() */
    if (server_fd >= 0) {
        close(server_fd);

        /* Wait for accept thread to finish (only if it was running and thread is valid) */
        if (was_running && thread != 0) {
            pthread_join(thread, NULL);
        }
    }

    /* Close all connections */
    for (int i = 0; i < MAX_NODES; i++) {
        if (ctx->network.nodes[i].sockfd >= 0) {
            close(ctx->network.nodes[i].sockfd);
            ctx->network.nodes[i].sockfd = -1;
            ctx->network.nodes[i].connected = false;
        }
    }

    LOG_INFO("Network shutdown complete");
}

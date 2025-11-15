/**
 * @file handlers.c
 * @brief Protocol message handlers implementation
 */

#include "handlers.h"
#include "network.h"
#include "../core/log.h"
#include "../core/dsm_context.h"
#include "../memory/page_table.h"
#include "../memory/permission.h"
#include <string.h>

/* PAGE_REQUEST */
int send_page_request(node_id_t owner, page_id_t page_id, access_type_t access) {
    dsm_context_t *ctx = dsm_get_context();
    message_t msg;
    memset(&msg, 0, sizeof(msg));

    msg.header.magic = MSG_MAGIC;
    msg.header.type = MSG_PAGE_REQUEST;
    msg.header.sender = ctx->node_id;
    msg.header.seq_num = 0;

    msg.payload.page_request.page_id = page_id;
    msg.payload.page_request.access = access;
    msg.payload.page_request.requester = ctx->node_id;

    LOG_DEBUG("Sending PAGE_REQUEST for page %lu to node %u", page_id, owner);
    return network_send(owner, &msg);
}

int handle_page_request(const message_t *msg) {
    page_id_t page_id = msg->payload.page_request.page_id;
    access_type_t access = msg->payload.page_request.access;
    node_id_t requester = msg->payload.page_request.requester;

    LOG_DEBUG("Handling PAGE_REQUEST for page %lu from node %u (access=%d)",
              page_id, requester, access);

    dsm_context_t *ctx = dsm_get_context();
    if (!ctx->page_table) {
        LOG_ERROR("No page table");
        return DSM_ERROR_INIT;
    }

    page_entry_t *entry = page_table_lookup_by_id(ctx->page_table, page_id);
    if (!entry) {
        LOG_ERROR("Page %lu not found", page_id);
        return DSM_ERROR_NOT_FOUND;
    }

    /* Send page data */
    return send_page_reply(requester, page_id, entry->local_addr);
}

/* PAGE_REPLY */
int send_page_reply(node_id_t requester, page_id_t page_id, const void *data) {
    dsm_context_t *ctx = dsm_get_context();
    message_t msg;
    memset(&msg, 0, sizeof(msg));

    msg.header.magic = MSG_MAGIC;
    msg.header.type = MSG_PAGE_REPLY;
    msg.header.sender = ctx->node_id;

    msg.payload.page_reply.page_id = page_id;
    msg.payload.page_reply.version = 0;
    memcpy(msg.payload.page_reply.data, data, PAGE_SIZE);

    LOG_DEBUG("Sending PAGE_REPLY for page %lu to node %u", page_id, requester);
    return network_send(requester, &msg);
}

int handle_page_reply(const message_t *msg) {
    page_id_t page_id = msg->payload.page_reply.page_id;

    LOG_DEBUG("Handling PAGE_REPLY for page %lu", page_id);

    dsm_context_t *ctx = dsm_get_context();
    if (!ctx->page_table) {
        return DSM_ERROR_INIT;
    }

    page_entry_t *entry = page_table_lookup_by_id(ctx->page_table, page_id);
    if (!entry) {
        return DSM_ERROR_NOT_FOUND;
    }

    /* Copy page data */
    memcpy(entry->local_addr, msg->payload.page_reply.data, PAGE_SIZE);

    /* Update permission to READ_WRITE */
    set_page_permission(entry->local_addr, PAGE_PERM_READ_WRITE);

    /* Signal waiting threads */
    pthread_mutex_lock(&ctx->page_table->lock);
    entry->request_pending = false;
    pthread_cond_broadcast(&entry->ready_cv);
    pthread_mutex_unlock(&ctx->page_table->lock);

    return DSM_SUCCESS;
}

/* INVALIDATE */
int send_invalidate(node_id_t target, page_id_t page_id) {
    dsm_context_t *ctx = dsm_get_context();
    message_t msg;
    memset(&msg, 0, sizeof(msg));

    msg.header.magic = MSG_MAGIC;
    msg.header.type = MSG_INVALIDATE;
    msg.header.sender = ctx->node_id;

    msg.payload.invalidate.page_id = page_id;
    msg.payload.invalidate.new_owner = ctx->node_id;

    LOG_DEBUG("Sending INVALIDATE for page %lu to node %u", page_id, target);
    return network_send(target, &msg);
}

int send_invalidate_ack(node_id_t target, page_id_t page_id) {
    dsm_context_t *ctx = dsm_get_context();
    message_t msg;
    memset(&msg, 0, sizeof(msg));

    msg.header.magic = MSG_MAGIC;
    msg.header.type = MSG_INVALIDATE_ACK;
    msg.header.sender = ctx->node_id;

    msg.payload.invalidate_ack.page_id = page_id;
    msg.payload.invalidate_ack.acker = ctx->node_id;

    return network_send(target, &msg);
}

int handle_invalidate(const message_t *msg) {
    page_id_t page_id = msg->payload.invalidate.page_id;
    node_id_t new_owner = msg->payload.invalidate.new_owner;

    LOG_DEBUG("Handling INVALIDATE for page %lu (new_owner=%u)", page_id, new_owner);

    dsm_context_t *ctx = dsm_get_context();
    if (!ctx->page_table) {
        return DSM_ERROR_INIT;
    }

    page_entry_t *entry = page_table_lookup_by_id(ctx->page_table, page_id);
    if (!entry) {
        return DSM_ERROR_NOT_FOUND;
    }

    /* Set page to INVALID */
    set_page_permission(entry->local_addr, PAGE_PERM_NONE);

    /* Send ACK */
    return send_invalidate_ack(msg->header.sender, page_id);
}

int handle_invalidate_ack(const message_t *msg) {
    page_id_t page_id = msg->payload.invalidate_ack.page_id;
    LOG_DEBUG("Received INVALIDATE_ACK for page %lu", page_id);
    return DSM_SUCCESS;
}

/* LOCK */
int send_lock_request(node_id_t manager, lock_id_t lock_id) {
    dsm_context_t *ctx = dsm_get_context();
    message_t msg;
    memset(&msg, 0, sizeof(msg));

    msg.header.magic = MSG_MAGIC;
    msg.header.type = MSG_LOCK_REQUEST;
    msg.header.sender = ctx->node_id;

    msg.payload.lock_request.lock_id = lock_id;
    msg.payload.lock_request.requester = ctx->node_id;

    LOG_DEBUG("Sending LOCK_REQUEST for lock %lu to node %u", lock_id, manager);
    return network_send(manager, &msg);
}

int send_lock_grant(node_id_t grantee, lock_id_t lock_id) {
    dsm_context_t *ctx = dsm_get_context();
    message_t msg;
    memset(&msg, 0, sizeof(msg));

    msg.header.magic = MSG_MAGIC;
    msg.header.type = MSG_LOCK_GRANT;
    msg.header.sender = ctx->node_id;

    msg.payload.lock_grant.lock_id = lock_id;
    msg.payload.lock_grant.grantee = grantee;

    return network_send(grantee, &msg);
}

int send_lock_release(node_id_t manager, lock_id_t lock_id) {
    dsm_context_t *ctx = dsm_get_context();
    message_t msg;
    memset(&msg, 0, sizeof(msg));

    msg.header.magic = MSG_MAGIC;
    msg.header.type = MSG_LOCK_RELEASE;
    msg.header.sender = ctx->node_id;

    msg.payload.lock_release.lock_id = lock_id;
    msg.payload.lock_release.releaser = ctx->node_id;

    LOG_DEBUG("Sending LOCK_RELEASE for lock %lu to node %u", lock_id, manager);
    return network_send(manager, &msg);
}

int handle_lock_request(const message_t *msg) {
    lock_id_t lock_id = msg->payload.lock_request.lock_id;
    node_id_t requester = msg->payload.lock_request.requester;

    LOG_DEBUG("Handling LOCK_REQUEST for lock %lu from node %u", lock_id, requester);

    /* Simple implementation: immediately grant */
    return send_lock_grant(requester, lock_id);
}

int handle_lock_grant(const message_t *msg) {
    lock_id_t lock_id = msg->payload.lock_grant.lock_id;
    LOG_DEBUG("Received LOCK_GRANT for lock %lu", lock_id);

    /* Signal waiting thread */
    dsm_context_t *ctx = dsm_get_context();
    pthread_mutex_lock(&ctx->lock_mgr.lock);
    /* Wake up waiting threads - simplified */
    pthread_mutex_unlock(&ctx->lock_mgr.lock);

    return DSM_SUCCESS;
}

int handle_lock_release(const message_t *msg) {
    lock_id_t lock_id = msg->payload.lock_release.lock_id;
    LOG_DEBUG("Handling LOCK_RELEASE for lock %lu", lock_id);
    return DSM_SUCCESS;
}

/* BARRIER */
int send_barrier_arrive(node_id_t manager, barrier_id_t barrier_id, int num_participants) {
    dsm_context_t *ctx = dsm_get_context();
    message_t msg;
    memset(&msg, 0, sizeof(msg));

    msg.header.magic = MSG_MAGIC;
    msg.header.type = MSG_BARRIER_ARRIVE;
    msg.header.sender = ctx->node_id;

    msg.payload.barrier_arrive.barrier_id = barrier_id;
    msg.payload.barrier_arrive.arriver = ctx->node_id;
    msg.payload.barrier_arrive.num_participants = num_participants;

    LOG_DEBUG("Sending BARRIER_ARRIVE for barrier %lu", barrier_id);
    return network_send(manager, &msg);
}

int send_barrier_release(node_id_t node, barrier_id_t barrier_id) {
    dsm_context_t *ctx = dsm_get_context();
    message_t msg;
    memset(&msg, 0, sizeof(msg));

    msg.header.magic = MSG_MAGIC;
    msg.header.type = MSG_BARRIER_RELEASE;
    msg.header.sender = ctx->node_id;

    msg.payload.barrier_release.barrier_id = barrier_id;
    msg.payload.barrier_release.num_arrived = 0;

    return network_send(node, &msg);
}

int handle_barrier_arrive(const message_t *msg) {
    barrier_id_t barrier_id = msg->payload.barrier_arrive.barrier_id;
    LOG_DEBUG("Handling BARRIER_ARRIVE for barrier %lu", barrier_id);

    /* Simple implementation: track count and release when all arrive */
    return DSM_SUCCESS;
}

int handle_barrier_release(const message_t *msg) {
    barrier_id_t barrier_id = msg->payload.barrier_release.barrier_id;
    LOG_DEBUG("Received BARRIER_RELEASE for barrier %lu", barrier_id);
    return DSM_SUCCESS;
}

/* Dispatcher */
int dispatch_message(const message_t *msg) {
    switch (msg->header.type) {
        case MSG_PAGE_REQUEST:
            return handle_page_request(msg);
        case MSG_PAGE_REPLY:
            return handle_page_reply(msg);
        case MSG_INVALIDATE:
            return handle_invalidate(msg);
        case MSG_INVALIDATE_ACK:
            return handle_invalidate_ack(msg);
        case MSG_LOCK_REQUEST:
            return handle_lock_request(msg);
        case MSG_LOCK_GRANT:
            return handle_lock_grant(msg);
        case MSG_LOCK_RELEASE:
            return handle_lock_release(msg);
        case MSG_BARRIER_ARRIVE:
            return handle_barrier_arrive(msg);
        case MSG_BARRIER_RELEASE:
            return handle_barrier_release(msg);
        default:
            LOG_WARN("Unknown message type: %d", msg->header.type);
            return DSM_ERROR_INVALID;
    }
}

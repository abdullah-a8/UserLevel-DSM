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
    if (ctx->num_allocations == 0) {
        LOG_ERROR("No allocations");
        return DSM_ERROR_INIT;
    }

    /* Search all page tables for this page ID
     * Hold ctx->lock to prevent race with dsm_free() compacting the array
     * Acquire reference before unlocking to prevent the table from being freed */
    page_entry_t *entry = NULL;
    page_table_t *owning_table = NULL;
    pthread_mutex_lock(&ctx->lock);
    for (int i = 0; i < ctx->num_allocations; i++) {
        if (ctx->page_tables[i]) {
            entry = page_table_lookup_by_id(ctx->page_tables[i], page_id);
            if (entry) {
                owning_table = ctx->page_tables[i];
                page_table_acquire(owning_table);  /* Acquire reference while holding ctx->lock */
                break;
            }
        }
    }
    pthread_mutex_unlock(&ctx->lock);

    if (!entry || !owning_table) {
        LOG_ERROR("Page %lu not found in any page table", page_id);
        return DSM_ERROR_NOT_FOUND;
    }

    /* Send page data */
    int rc = send_page_reply(requester, page_id, entry->local_addr);
    if (rc != DSM_SUCCESS) {
        page_table_release(owning_table);
        return rc;
    }

    /* Update stats */
    pthread_mutex_lock(&ctx->stats_lock);
    ctx->stats.pages_sent++;
    pthread_mutex_unlock(&ctx->stats_lock);

    /* If request is for WRITE access, downgrade our copy */
    if (access == ACCESS_WRITE) {
        LOG_DEBUG("Downgrading page %lu to INVALID (transferred to node %u)",
                  page_id, requester);

        /* Check return value of set_page_permission */
        rc = set_page_permission(entry->local_addr, PAGE_PERM_NONE);
        if (rc != DSM_SUCCESS) {
            LOG_ERROR("Failed to set page %lu permission to NONE", page_id);
            page_table_release(owning_table);
            return rc;
        }

        /* Acquire page table lock before modifying entry state/owner */
        pthread_mutex_lock(&owning_table->lock);
        entry->state = PAGE_STATE_INVALID;
        entry->owner = requester;
        pthread_mutex_unlock(&owning_table->lock);
    } else {
        /* For READ access, we keep our copy and can also share */
        LOG_DEBUG("Keeping page %lu as shared (node %u also has read access)",
                  page_id, requester);

        pthread_mutex_lock(&owning_table->lock);
        if (entry->state == PAGE_STATE_READ_WRITE) {
            pthread_mutex_unlock(&owning_table->lock);

            /* Check return value of set_page_permission */
            rc = set_page_permission(entry->local_addr, PAGE_PERM_READ);
            if (rc != DSM_SUCCESS) {
                LOG_ERROR("Failed to set page %lu permission to READ", page_id);
                page_table_release(owning_table);
                return rc;
            }

            /* Downgrade to READ_ONLY since someone else has a copy */
            pthread_mutex_lock(&owning_table->lock);
            entry->state = PAGE_STATE_READ_ONLY;
            pthread_mutex_unlock(&owning_table->lock);
        } else {
            pthread_mutex_unlock(&owning_table->lock);
        }
    }

    page_table_release(owning_table);
    return DSM_SUCCESS;
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
    uint64_t version = msg->payload.page_reply.version;

    LOG_DEBUG("Handling PAGE_REPLY for page %lu (version %lu)", page_id, version);

    dsm_context_t *ctx = dsm_get_context();
    if (ctx->num_allocations == 0) {
        return DSM_ERROR_INIT;
    }

    /* Search all page tables for this page ID
     * Hold ctx->lock to prevent race with dsm_free() compacting the array
     * Acquire reference before unlocking to prevent the table from being freed */
    page_entry_t *entry = NULL;
    page_table_t *owning_table = NULL;
    pthread_mutex_lock(&ctx->lock);
    for (int i = 0; i < ctx->num_allocations; i++) {
        if (ctx->page_tables[i]) {
            entry = page_table_lookup_by_id(ctx->page_tables[i], page_id);
            if (entry) {
                owning_table = ctx->page_tables[i];
                page_table_acquire(owning_table);  /* Acquire reference while holding ctx->lock */
                break;
            }
        }
    }
    pthread_mutex_unlock(&ctx->lock);

    if (!entry || !owning_table) {
        LOG_ERROR("Page %lu not found in any page table", page_id);
        return DSM_ERROR_NOT_FOUND;
    }

    /* Copy page data */
    memcpy(entry->local_addr, msg->payload.page_reply.data, PAGE_SIZE);
    entry->version = version;

    /* Set appropriate permission - will be set by fetch_page_read/write */
    /* For now, conservatively set to READ_WRITE */
    set_page_permission(entry->local_addr, PAGE_PERM_READ_WRITE);

    LOG_DEBUG("Copied page %lu data and updated permissions", page_id);

    /* Signal waiting threads (Task 8.1: wake all queued requesters) */
    pthread_mutex_lock(&entry->entry_lock);
    int waiters = entry->num_waiting_threads;
    entry->request_pending = false;
    pthread_cond_broadcast(&entry->ready_cv);  /* Wake ALL waiting threads */
    pthread_mutex_unlock(&entry->entry_lock);

    if (waiters > 0) {
        LOG_DEBUG("Woke %d waiting threads for page %lu", waiters, page_id);
    }

    page_table_release(owning_table);
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

    /* Search all page tables for this page ID
     * Hold ctx->lock to prevent race with dsm_free() compacting the array
     * Acquire reference before unlocking to prevent the table from being freed */
    page_entry_t *entry = NULL;
    page_table_t *owning_table = NULL;
    pthread_mutex_lock(&ctx->lock);
    for (int i = 0; i < ctx->num_allocations; i++) {
        if (ctx->page_tables[i]) {
            entry = page_table_lookup_by_id(ctx->page_tables[i], page_id);
            if (entry) {
                owning_table = ctx->page_tables[i];
                page_table_acquire(owning_table);  /* Acquire reference while holding ctx->lock */
                break;
            }
        }
    }
    pthread_mutex_unlock(&ctx->lock);

    if (!entry || !owning_table) {
        LOG_WARN("Page %lu not found in any table, ignoring invalidation", page_id);
        /* Still send ACK even if page not found */
        return send_invalidate_ack(msg->header.sender, page_id);
    }

    /* Update stats */
    pthread_mutex_lock(&ctx->stats_lock);
    ctx->stats.invalidations_received++;
    pthread_mutex_unlock(&ctx->stats_lock);

    /* Set page to INVALID */
    int rc = set_page_permission(entry->local_addr, PAGE_PERM_NONE);
    if (rc != DSM_SUCCESS) {
        LOG_ERROR("Failed to set page %lu permission to NONE", page_id);
    }

    /* Acquire page table lock before modifying entry state/owner */
    pthread_mutex_lock(&owning_table->lock);
    entry->state = PAGE_STATE_INVALID;
    entry->owner = new_owner;
    pthread_mutex_unlock(&owning_table->lock);

    LOG_DEBUG("Invalidated page %lu (state=INVALID, new_owner=%u)",
              page_id, new_owner);

    /* Send ACK */
    page_table_release(owning_table);
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

    /* Forward to lock manager (implemented in sync/lock.c) */
    extern int lock_manager_grant(lock_id_t lock_id, node_id_t requester);
    return lock_manager_grant(lock_id, requester);
}

int handle_lock_grant(const message_t *msg) {
    lock_id_t lock_id = msg->payload.lock_grant.lock_id;
    node_id_t grantee = msg->payload.lock_grant.grantee;

    LOG_DEBUG("Received LOCK_GRANT for lock %lu (grantee=%u)", lock_id, grantee);

    /* Forward to lock handler (implemented in sync/lock.c) */
    extern int lock_handle_grant(lock_id_t lock_id, node_id_t grantee);
    return lock_handle_grant(lock_id, grantee);
}

int handle_lock_release(const message_t *msg) {
    lock_id_t lock_id = msg->payload.lock_release.lock_id;
    node_id_t releaser = msg->payload.lock_release.releaser;

    LOG_DEBUG("Handling LOCK_RELEASE for lock %lu from node %u", lock_id, releaser);

    /* Forward to lock manager (implemented in sync/lock.c) */
    extern int lock_manager_release(lock_id_t lock_id, node_id_t releaser);
    return lock_manager_release(lock_id, releaser);
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
    node_id_t arriver = msg->payload.barrier_arrive.arriver;
    int num_participants = msg->payload.barrier_arrive.num_participants;

    LOG_DEBUG("Handling BARRIER_ARRIVE for barrier %lu from node %u (%d participants)",
              barrier_id, arriver, num_participants);

    /* Forward to barrier manager (implemented in sync/barrier.c) */
    extern int barrier_manager_arrive(barrier_id_t barrier_id, node_id_t arriver, int num_participants);
    return barrier_manager_arrive(barrier_id, arriver, num_participants);
}

int handle_barrier_release(const message_t *msg) {
    barrier_id_t barrier_id = msg->payload.barrier_release.barrier_id;
    LOG_DEBUG("Received BARRIER_RELEASE for barrier %lu", barrier_id);

    /* Forward to barrier handler (implemented in sync/barrier.c) */
    extern int barrier_handle_release(barrier_id_t barrier_id);
    return barrier_handle_release(barrier_id);
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

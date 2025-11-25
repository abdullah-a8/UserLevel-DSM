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
#include "../consistency/directory.h"
#include "../consistency/page_migration.h"
#include <string.h>
#include <errno.h>
#include <sys/mman.h>
#include <unistd.h>
#include <time.h>

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

    /* CRITICAL: Validate page state before serving
     * Do not serve pages that have been invalidated
     * This prevents serving stale data when directory is out of sync */
    pthread_mutex_lock(&owning_table->lock);
    page_state_t current_state = entry->state;
    pthread_mutex_unlock(&owning_table->lock);

    if (current_state == PAGE_STATE_INVALID) {
        LOG_ERROR("Cannot serve page %lu - page is INVALID (no longer owner)", page_id);
        page_table_release(owning_table);
        /* Send error message to requester */
        message_t err_msg;
        memset(&err_msg, 0, sizeof(err_msg));
        err_msg.header.magic = MSG_MAGIC;
        err_msg.header.type = MSG_ERROR;
        err_msg.header.sender = ctx->node_id;
        err_msg.payload.error.error_code = DSM_ERROR_INVALID;
        err_msg.payload.error.page_id = page_id;
        snprintf(err_msg.payload.error.error_msg, 256,
                 "Page %lu is INVALID - requester should retry directory lookup", page_id);
        network_send(requester, &err_msg);
        return DSM_ERROR_INVALID;
    }

    /* Send page data with the requested access type */
    int rc = send_page_reply(requester, page_id, access, entry->local_addr);
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

        /* CRITICAL FIX (BUG #8): Track requester as sharer in owner's directory
         * This ensures future writers can get complete sharer list for invalidations */
        page_directory_t *dir = get_page_directory();
        if (dir) {
            rc = directory_add_reader(dir, page_id, requester);
            if (rc != DSM_SUCCESS && rc != DSM_ERROR_BUSY) {
                LOG_WARN("Failed to add node %u as sharer for page %lu", requester, page_id);
            } else {
                LOG_DEBUG("Tracked node %u as sharer for page %lu in owner's directory",
                         requester, page_id);
            }
        }
    }

    page_table_release(owning_table);
    return DSM_SUCCESS;
}

/* PAGE_REPLY */
int send_page_reply(node_id_t requester, page_id_t page_id, access_type_t access, const void *data) {
    dsm_context_t *ctx = dsm_get_context();
    message_t msg;
    memset(&msg, 0, sizeof(msg));

    msg.header.magic = MSG_MAGIC;
    msg.header.type = MSG_PAGE_REPLY;
    msg.header.sender = ctx->node_id;

    msg.payload.page_reply.page_id = page_id;
    msg.payload.page_reply.version = 0;
    msg.payload.page_reply.access = access;
    memcpy(msg.payload.page_reply.data, data, PAGE_SIZE);

    LOG_DEBUG("Sending PAGE_REPLY for page %lu to node %u (access=%s)",
              page_id, requester, access == ACCESS_READ ? "READ" : "WRITE");
    return network_send(requester, &msg);
}

int handle_page_reply(const message_t *msg) {
    page_id_t page_id = msg->payload.page_reply.page_id;
    uint64_t version = msg->payload.page_reply.version;
    access_type_t access = msg->payload.page_reply.access;

    LOG_DEBUG("Handling PAGE_REPLY for page %lu (version %lu, access=%s)",
              page_id, version, access == ACCESS_READ ? "READ" : "WRITE");

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

    /* CRITICAL FIX: Temporarily enable write access for memcpy
     * The dispatcher thread handles PAGE_REPLY and must copy data into shared memory.
     * If the page is in NO_ACCESS or READ_ONLY state, memcpy will trigger a write fault
     * ON THE DISPATCHER THREAD, causing a deadlock (dispatcher can't receive its own replies).
     * Solution: Temporarily grant write access, copy data, then set final permission. */
    int rc = set_page_permission(entry->local_addr, PAGE_PERM_READ_WRITE);
    if (rc != DSM_SUCCESS) {
        LOG_ERROR("Failed to grant temporary write access for page %lu data copy", page_id);
        page_table_release(owning_table);
        return rc;
    }

    /* Copy page data */
    memcpy(entry->local_addr, msg->payload.page_reply.data, PAGE_SIZE);
    entry->version = version;

    /* Set appropriate permission based on requested access type
     * This ensures coherence protocol correctness:
     * - READ access -> READ_ONLY permission (PROT_READ)
     * - WRITE access -> READ_WRITE permission (PROT_READ|PROT_WRITE)
     */
    page_perm_t permission = (access == ACCESS_READ) ? PAGE_PERM_READ : PAGE_PERM_READ_WRITE;
    rc = set_page_permission(entry->local_addr, permission);
    if (rc != DSM_SUCCESS) {
        LOG_ERROR("Failed to set page %lu permission to %s",
                  page_id, permission == PAGE_PERM_READ ? "READ" : "READ_WRITE");
        page_table_release(owning_table);
        return rc;
    }

    LOG_DEBUG("Copied page %lu data and set permission to %s",
              page_id, permission == PAGE_PERM_READ ? "READ" : "READ_WRITE");

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

    /* Update directory to reflect new owner
     * This ensures all nodes maintain consistent ownership information
     * Critical for coherence protocol correctness */
    page_directory_t *dir = get_page_directory();
    if (dir) {
        directory_set_owner(dir, page_id, new_owner);
        directory_remove_sharer(dir, page_id, ctx->node_id);
        LOG_DEBUG("Updated directory: page %lu now owned by node %u", page_id, new_owner);
    }

    LOG_DEBUG("Invalidated page %lu (state=INVALID, new_owner=%u)",
              page_id, new_owner);

    /* Send ACK */
    page_table_release(owning_table);
    return send_invalidate_ack(msg->header.sender, page_id);
}

int handle_invalidate_ack(const message_t *msg) {
    page_id_t page_id = msg->payload.invalidate_ack.page_id;
    node_id_t acker = msg->payload.invalidate_ack.acker;

    LOG_DEBUG("Received INVALIDATE_ACK for page %lu from node %u", page_id, acker);

    dsm_context_t *ctx = dsm_get_context();

    /* Find the page entry to decrement pending ACK counter */
    page_entry_t *entry = NULL;
    pthread_mutex_lock(&ctx->lock);
    for (int i = 0; i < ctx->num_allocations; i++) {
        if (ctx->page_tables[i]) {
            entry = page_table_lookup_by_id(ctx->page_tables[i], page_id);
            if (entry) break;
        }
    }
    pthread_mutex_unlock(&ctx->lock);

    if (!entry) {
        LOG_WARN("Received INVALIDATE_ACK for unknown page %lu", page_id);
        return DSM_SUCCESS;
    }

    /* Decrement pending ACK counter and signal if all ACKs received */
    pthread_mutex_lock(&entry->entry_lock);
    if (entry->pending_inv_acks > 0) {
        entry->pending_inv_acks--;
        LOG_DEBUG("Page %lu: pending_inv_acks decremented to %d",
                  page_id, entry->pending_inv_acks);

        if (entry->pending_inv_acks == 0) {
            /* All ACKs received, wake up waiting thread */
            pthread_cond_signal(&entry->inv_ack_cv);
            LOG_DEBUG("All invalidation ACKs received for page %lu, signaling", page_id);
        }
    }
    pthread_mutex_unlock(&entry->entry_lock);

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

    LOG_INFO("Sending BARRIER_RELEASE for barrier %lu to node %u", barrier_id, node);
    int rc = network_send(node, &msg);
    if (rc != DSM_SUCCESS) {
        LOG_ERROR("Failed to send BARRIER_RELEASE to node %u (rc=%d)", node, rc);
    }
    return rc;
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
    LOG_INFO("Received BARRIER_RELEASE for barrier %lu from node %u",
             barrier_id, msg->header.sender);

    /* Forward to barrier handler (implemented in sync/barrier.c) */
    extern int barrier_handle_release(barrier_id_t barrier_id);
    return barrier_handle_release(barrier_id);
}

/* ALLOC_NOTIFY */
int send_alloc_notify(page_id_t start_page_id, page_id_t end_page_id, node_id_t owner, size_t num_pages, void *base_addr, size_t total_size) {
    dsm_context_t *ctx = dsm_get_context();

    /* Broadcast to all connected nodes */
    for (int i = 0; i < MAX_NODES; i++) {
        pthread_mutex_lock(&ctx->lock);
        bool connected = ctx->network.nodes[i].connected;
        node_id_t node_id = ctx->network.nodes[i].id;
        pthread_mutex_unlock(&ctx->lock);

        if (connected && node_id != ctx->node_id) {
            message_t msg;
            memset(&msg, 0, sizeof(msg));

            msg.header.magic = MSG_MAGIC;
            msg.header.type = MSG_ALLOC_NOTIFY;
            msg.header.sender = ctx->node_id;

            msg.payload.alloc_notify.start_page_id = start_page_id;
            msg.payload.alloc_notify.end_page_id = end_page_id;
            msg.payload.alloc_notify.owner = owner;
            msg.payload.alloc_notify.num_pages = num_pages;
            msg.payload.alloc_notify.base_addr = (uint64_t)base_addr;
            msg.payload.alloc_notify.total_size = total_size;

            LOG_INFO("Sending ALLOC_NOTIFY to node %u (pages %lu-%lu, addr=%p, size=%zu, owner=%u)",
                     node_id, start_page_id, end_page_id, base_addr, total_size, owner);

            int rc = network_send(node_id, &msg);
            if (rc != DSM_SUCCESS) {
                LOG_WARN("Failed to send ALLOC_NOTIFY to node %u", node_id);
            }
        }
    }

    return DSM_SUCCESS;
}

int handle_alloc_notify(const message_t *msg) {
    page_id_t start_page_id = msg->payload.alloc_notify.start_page_id;
    page_id_t end_page_id = msg->payload.alloc_notify.end_page_id;
    node_id_t owner = msg->payload.alloc_notify.owner;
    size_t num_pages = msg->payload.alloc_notify.num_pages;
    void *base_addr = (void*)msg->payload.alloc_notify.base_addr;
    size_t total_size = msg->payload.alloc_notify.total_size;

    LOG_INFO("Received ALLOC_NOTIFY: pages %lu-%lu at addr=%p, size=%zu, owned by node %u",
             start_page_id, end_page_id, base_addr, total_size, owner);

    dsm_context_t *ctx = dsm_get_context();

    /* CRITICAL: Create mmap at the SAME virtual address for SVAS
     * This ensures all nodes share the same virtual address space */
    void *addr = mmap(base_addr, total_size, PROT_NONE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);

    if (addr == MAP_FAILED || addr != base_addr) {
        LOG_ERROR("Failed to create SVAS mapping at %p (got %p): %s",
                  base_addr, addr, strerror(errno));
        return DSM_ERROR_MEMORY;
    }

    LOG_INFO("Created SVAS mapping at %p (size=%zu) for remote allocation",
             addr, total_size);

    /* Create page table for this remote allocation */
    pthread_mutex_lock(&ctx->lock);

    if (ctx->num_allocations >= 32) {
        pthread_mutex_unlock(&ctx->lock);
        munmap(addr, total_size);
        LOG_ERROR("Maximum number of allocations (32) exceeded");
        return DSM_ERROR_MEMORY;
    }

    /* Create page table with the SAME page IDs as owner */
    page_table_t *new_table = page_table_create_remote(addr, total_size, owner, start_page_id);
    if (!new_table) {
        pthread_mutex_unlock(&ctx->lock);
        munmap(addr, total_size);
        LOG_ERROR("Failed to create remote page table");
        return DSM_ERROR_MEMORY;
    }

    /* Add to list of page tables */
    ctx->page_tables[ctx->num_allocations] = new_table;
    ctx->num_allocations++;

    /* Initialize page directory if needed */
    page_directory_t *dir = get_page_directory();
    if (!dir) {
        /* First remote allocation - initialize directory */
        int rc = consistency_init(100000);
        if (rc != DSM_SUCCESS && rc != DSM_ERROR_INIT) {
            LOG_ERROR("Failed to initialize consistency module");
            page_table_destroy(new_table);
            ctx->page_tables[ctx->num_allocations - 1] = NULL;
            ctx->num_allocations--;
            pthread_mutex_unlock(&ctx->lock);
            munmap(addr, total_size);
            return rc;
        }
        dir = get_page_directory();
    }

    /* Register pages in directory with remote owner */
    if (dir) {
        for (page_id_t page_id = start_page_id; page_id <= end_page_id; page_id++) {
            directory_set_owner(dir, page_id, owner);
        }
        LOG_DEBUG("Registered %zu remote pages (owner=node %u) in directory",
                  num_pages, owner);
    }

    pthread_mutex_unlock(&ctx->lock);

    LOG_INFO("SVAS setup complete: local addr=%p maps to remote pages %lu-%lu (owner=node %u)",
             addr, start_page_id, end_page_id, owner);

    /* CRITICAL FIX #4: Manager forwards ALLOC_NOTIFY to all other workers
     * This enables multi-node (3+) configurations where workers allocate their own partitions.
     * Without forwarding, workers only notify the manager, so other workers never learn
     * about the allocation and dsm_get_allocation() fails.
     *
     * Star topology: Workers only connect to manager, not to each other.
     * When worker N allocates, it sends ALLOC_NOTIFY only to manager (node 0).
     * Manager must forward to all other workers so they can create SVAS mappings.
     */
    node_id_t sender = msg->header.sender;
    if (ctx->config.is_manager && sender != ctx->node_id) {
        LOG_INFO("Manager forwarding ALLOC_NOTIFY from node %u to other workers", sender);

        /* Forward to all connected workers except sender and self */
        int forwarded = 0;
        for (int i = 0; i < MAX_NODES; i++) {
            pthread_mutex_lock(&ctx->lock);
            bool connected = ctx->network.nodes[i].connected;
            node_id_t target_id = ctx->network.nodes[i].id;
            pthread_mutex_unlock(&ctx->lock);

            if (connected && target_id != ctx->node_id && target_id != sender) {
                LOG_INFO("Forwarding ALLOC_NOTIFY to node %u (pages %lu-%lu, owner=%u)",
                         target_id, start_page_id, end_page_id, owner);

                /* Create a copy of the message to forward (preserve original sender) */
                message_t forward_msg;
                memcpy(&forward_msg, msg, sizeof(message_t));

                int rc = network_send(target_id, &forward_msg);
                if (rc != DSM_SUCCESS) {
                    LOG_WARN("Failed to forward ALLOC_NOTIFY to node %u", target_id);
                } else {
                    forwarded++;
                }
            }
        }

        LOG_INFO("Manager forwarded ALLOC_NOTIFY to %d workers", forwarded);
    }

    /* CRITICAL FIX #1: Send ALLOC_ACK to allocation owner after SVAS setup */
    return send_alloc_ack(owner, start_page_id, end_page_id);
}

/* ALLOC_ACK */
int send_alloc_ack(node_id_t target, page_id_t start_page_id, page_id_t end_page_id) {
    dsm_context_t *ctx = dsm_get_context();
    message_t msg;
    memset(&msg, 0, sizeof(msg));

    msg.header.magic = MSG_MAGIC;
    msg.header.type = MSG_ALLOC_ACK;
    msg.header.sender = ctx->node_id;

    msg.payload.alloc_ack.start_page_id = start_page_id;
    msg.payload.alloc_ack.end_page_id = end_page_id;
    msg.payload.alloc_ack.acker = ctx->node_id;

    LOG_DEBUG("Sending ALLOC_ACK to node %u for pages %lu-%lu",
              target, start_page_id, end_page_id);

    return network_send(target, &msg);
}

int handle_alloc_ack(const message_t *msg) {
    page_id_t start_page_id = msg->payload.alloc_ack.start_page_id;
    page_id_t end_page_id = msg->payload.alloc_ack.end_page_id;
    node_id_t acker = msg->payload.alloc_ack.acker;

    LOG_DEBUG("Received ALLOC_ACK from node %u for pages %lu-%lu",
              acker, start_page_id, end_page_id);

    dsm_context_t *ctx = dsm_get_context();
    alloc_ack_tracker_t *tracker = &ctx->network.alloc_tracker;

    pthread_mutex_lock(&tracker->lock);

    /* Check if we're tracking this allocation */
    if (!tracker->active) {
        pthread_mutex_unlock(&tracker->lock);
        LOG_WARN("Received ALLOC_ACK but no allocation is being tracked");
        return DSM_SUCCESS;
    }

    /* Verify this ACK matches the tracked allocation */
    if (tracker->start_page_id != start_page_id ||
        tracker->end_page_id != end_page_id) {
        pthread_mutex_unlock(&tracker->lock);
        LOG_WARN("ALLOC_ACK page range mismatch: expected %lu-%lu, got %lu-%lu",
                 tracker->start_page_id, tracker->end_page_id,
                 start_page_id, end_page_id);
        return DSM_SUCCESS;
    }

    /* Mark this node as having ACK'd */
    if (acker < MAX_NODES && !tracker->acks_received[acker]) {
        tracker->acks_received[acker] = true;
        tracker->received_acks++;
        LOG_DEBUG("ALLOC_ACK received from node %u (%d/%d)",
                  acker, tracker->received_acks, tracker->expected_acks);

        /* If all ACKs received, signal waiting thread */
        if (tracker->received_acks >= tracker->expected_acks) {
            LOG_INFO("All %d ALLOC_ACKs received for pages %lu-%lu, signaling",
                     tracker->expected_acks, start_page_id, end_page_id);
            pthread_cond_signal(&tracker->all_acks_cv);
        }
    }

    pthread_mutex_unlock(&tracker->lock);
    return DSM_SUCCESS;
}

int wait_for_alloc_acks(page_id_t start_page_id, page_id_t end_page_id,
                        int expected_acks, int timeout_sec) {
    if (expected_acks == 0) {
        return DSM_SUCCESS;  /* No ACKs to wait for */
    }

    dsm_context_t *ctx = dsm_get_context();
    alloc_ack_tracker_t *tracker = &ctx->network.alloc_tracker;

    pthread_mutex_lock(&tracker->lock);

    /* Initialize tracker */
    tracker->start_page_id = start_page_id;
    tracker->end_page_id = end_page_id;
    tracker->expected_acks = expected_acks;
    tracker->received_acks = 0;
    tracker->active = true;
    for (int i = 0; i < MAX_NODES; i++) {
        tracker->acks_received[i] = false;
    }

    LOG_INFO("Waiting for %d ALLOC_ACKs for pages %lu-%lu (timeout=%ds)",
             expected_acks, start_page_id, end_page_id, timeout_sec);

    /* Wait for all ACKs with timeout */
    struct timespec timeout;
    clock_gettime(CLOCK_REALTIME, &timeout);
    timeout.tv_sec += timeout_sec;

    int rc = DSM_SUCCESS;
    while (tracker->received_acks < tracker->expected_acks) {
        int wait_rc = pthread_cond_timedwait(&tracker->all_acks_cv,
                                             &tracker->lock, &timeout);
        if (wait_rc == ETIMEDOUT) {
            LOG_ERROR("Timeout waiting for ALLOC_ACKs (received %d/%d)",
                      tracker->received_acks, tracker->expected_acks);
            rc = DSM_ERROR_TIMEOUT;
            break;
        }
    }

    /* Deactivate tracker */
    tracker->active = false;
    pthread_mutex_unlock(&tracker->lock);

    if (rc == DSM_SUCCESS) {
        LOG_INFO("Successfully received all %d ALLOC_ACKs", expected_acks);
    }

    return rc;
}

/* NODE_JOIN */
int send_node_join(node_id_t node_id, const char *hostname, uint16_t port) {
    message_t msg;
    memset(&msg, 0, sizeof(msg));

    msg.header.magic = MSG_MAGIC;
    msg.header.type = MSG_NODE_JOIN;
    msg.header.sender = node_id;

    msg.payload.node_join.node_id = node_id;
    msg.payload.node_join.port = port;
    if (hostname) {
        strncpy(msg.payload.node_join.hostname, hostname, MAX_HOSTNAME_LEN - 1);
    }

    LOG_INFO("Sending NODE_JOIN to manager (node_id=%u, port=%u)", node_id, port);

    /* Send to manager (node 0) */
    return network_send(0, &msg);
}

int handle_node_join(const message_t *msg, int sockfd) {
    node_id_t joining_node_id = msg->payload.node_join.node_id;
    uint16_t port = msg->payload.node_join.port;
    const char *hostname = msg->payload.node_join.hostname;

    LOG_INFO("Handling NODE_JOIN from node %u (port=%u, hostname=%s, sockfd=%d)",
             joining_node_id, port, hostname, sockfd);

    dsm_context_t *ctx = dsm_get_context();

    /* Validate node_id */
    if (joining_node_id >= MAX_NODES) {
        LOG_ERROR("Invalid node_id %u (max=%d)", joining_node_id, MAX_NODES);
        return DSM_ERROR_INVALID;
    }

    /* Remove from pending connections list */
    pthread_mutex_lock(&ctx->network.pending_lock);
    for (int i = 0; i < ctx->network.num_pending; i++) {
        if (ctx->network.pending_sockets[i] == sockfd) {
            /* Remove by shifting remaining elements */
            for (int j = i; j < ctx->network.num_pending - 1; j++) {
                ctx->network.pending_sockets[j] = ctx->network.pending_sockets[j + 1];
            }
            ctx->network.num_pending--;
            LOG_DEBUG("Removed sockfd=%d from pending (remaining=%d)",
                     sockfd, ctx->network.num_pending);
            break;
        }
    }
    pthread_mutex_unlock(&ctx->network.pending_lock);

    pthread_mutex_lock(&ctx->lock);

    /* Check if this node is already connected */
    if (ctx->network.nodes[joining_node_id].connected) {
        LOG_WARN("Node %u already connected, updating connection", joining_node_id);
        /* Close old socket if different */
        if (ctx->network.nodes[joining_node_id].sockfd != sockfd &&
            ctx->network.nodes[joining_node_id].sockfd >= 0) {
            close(ctx->network.nodes[joining_node_id].sockfd);
        }
    } else {
        ctx->network.num_nodes++;
    }

    /* Store connection with CORRECT node_id mapping */
    ctx->network.nodes[joining_node_id].sockfd = sockfd;
    ctx->network.nodes[joining_node_id].connected = true;
    ctx->network.nodes[joining_node_id].id = joining_node_id;
    ctx->network.nodes[joining_node_id].port = port;
    if (hostname[0] != '\0') {
        strncpy(ctx->network.nodes[joining_node_id].hostname, hostname, MAX_HOSTNAME_LEN - 1);
    }

    /* CRITICAL FIX: Initialize heartbeat timestamp to prevent immediate failure detection */
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    ctx->network.nodes[joining_node_id].last_heartbeat_time = now.tv_sec * 1000000000ULL + now.tv_nsec;
    ctx->network.nodes[joining_node_id].missed_heartbeats = 0;
    ctx->network.nodes[joining_node_id].is_failed = false;

    pthread_mutex_unlock(&ctx->lock);

    LOG_INFO("Node %u successfully joined (sockfd=%d, total_nodes=%d)",
             joining_node_id, sockfd, ctx->network.num_nodes);

    return DSM_SUCCESS;
}

/* HEARTBEAT */
int send_heartbeat(node_id_t target) {
    dsm_context_t *ctx = dsm_get_context();
    message_t msg;
    memset(&msg, 0, sizeof(msg));

    msg.header.magic = MSG_MAGIC;
    msg.header.type = MSG_HEARTBEAT;
    msg.header.sender = ctx->node_id;

    return network_send(target, &msg);
}

int handle_heartbeat(const message_t *msg) {
    /* CRITICAL FIX #2: Update last_heartbeat_time to track node health */
    dsm_context_t *ctx = dsm_get_context();
    node_id_t sender = msg->header.sender;

    LOG_DEBUG("Received HEARTBEAT from node %u", sender);

    if (sender >= MAX_NODES) {
        return DSM_ERROR_INVALID;
    }

    pthread_mutex_lock(&ctx->lock);
    if (ctx->network.nodes[sender].connected) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        ctx->network.nodes[sender].last_heartbeat_time =
            now.tv_sec * 1000000000ULL + now.tv_nsec;
        ctx->network.nodes[sender].missed_heartbeats = 0;

        /* If node was marked as failed, resurrect it */
        if (ctx->network.nodes[sender].is_failed) {
            LOG_INFO("Node %u has recovered (heartbeat received)", sender);
            ctx->network.nodes[sender].is_failed = false;
        }
    }
    pthread_mutex_unlock(&ctx->lock);

    return DSM_SUCCESS;
}

/* CRITICAL FIX #2: Heartbeat thread for failure detection */
static void* heartbeat_thread_func(void *arg) {
    (void)arg;
    dsm_context_t *ctx = dsm_get_context();

    const uint64_t HEARTBEAT_TIMEOUT_NS = 6000000000ULL;   /* 6 seconds (3 missed) */
    const int MAX_MISSED_HEARTBEATS = 3;

    LOG_INFO("Heartbeat thread started (interval=2s, timeout=6s)");

    while (ctx->network.running) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        uint64_t current_time = now.tv_sec * 1000000000ULL + now.tv_nsec;

        pthread_mutex_lock(&ctx->lock);

        /* Send heartbeats to all connected nodes */
        for (int i = 0; i < MAX_NODES; i++) {
            if (ctx->network.nodes[i].connected &&
                ctx->network.nodes[i].id != ctx->node_id &&
                !ctx->network.nodes[i].is_failed) {

                /* Send heartbeat */
                node_id_t target = ctx->network.nodes[i].id;
                pthread_mutex_unlock(&ctx->lock);  /* Release lock during send */
                send_heartbeat(target);
                pthread_mutex_lock(&ctx->lock);  /* Reacquire lock */
            }
        }

        /* Check for failed nodes (nodes that haven't sent heartbeats) */
        for (int i = 0; i < MAX_NODES; i++) {
            if (ctx->network.nodes[i].connected &&
                ctx->network.nodes[i].id != ctx->node_id &&
                !ctx->network.nodes[i].is_failed) {

                uint64_t last_hb = ctx->network.nodes[i].last_heartbeat_time;

                /* If this is the first heartbeat, initialize timestamp */
                if (last_hb == 0) {
                    ctx->network.nodes[i].last_heartbeat_time = current_time;
                    continue;
                }

                uint64_t elapsed = current_time - last_hb;
                if (elapsed > HEARTBEAT_TIMEOUT_NS) {
                    ctx->network.nodes[i].missed_heartbeats++;

                    if (ctx->network.nodes[i].missed_heartbeats >= MAX_MISSED_HEARTBEATS) {
                        /* Mark node as failed */
                        ctx->network.nodes[i].is_failed = true;
                        node_id_t failed_node_id = ctx->network.nodes[i].id;
                        LOG_ERROR("Node %u marked as FAILED (no heartbeat for %llu ms)",
                                  failed_node_id, elapsed / 1000000);

                        /* CRITICAL FIX: Trigger recovery - clean up directory entries */
                        page_directory_t *dir = get_page_directory();
                        if (dir) {
                            /* Release lock during directory operation to avoid deadlock */
                            pthread_mutex_unlock(&ctx->lock);
                            directory_handle_node_failure(dir, failed_node_id);
                            
                            /* Broadcast failure to others */
                            broadcast_node_failure(failed_node_id);

                            pthread_mutex_lock(&ctx->lock);
                            /* Note: Need to recheck loop condition after reacquiring lock */
                        }
                    }
                }
            }
        }

        pthread_mutex_unlock(&ctx->lock);

        /* Sleep for heartbeat interval */
        struct timespec sleep_time = {
            .tv_sec = 2,
            .tv_nsec = 0
        };
        nanosleep(&sleep_time, NULL);
    }

    LOG_INFO("Heartbeat thread stopped");
    return NULL;
}

void start_heartbeat_thread(void) {
    dsm_context_t *ctx = dsm_get_context();

    /* Initialize all nodes' heartbeat timestamps */
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t current_time = now.tv_sec * 1000000000ULL + now.tv_nsec;

    pthread_mutex_lock(&ctx->lock);
    for (int i = 0; i < MAX_NODES; i++) {
        ctx->network.nodes[i].last_heartbeat_time = current_time;
        ctx->network.nodes[i].missed_heartbeats = 0;
        ctx->network.nodes[i].is_failed = false;
    }
    pthread_mutex_unlock(&ctx->lock);

    if (pthread_create(&ctx->network.heartbeat_thread, NULL,
                       heartbeat_thread_func, NULL) != 0) {
        LOG_ERROR("Failed to create heartbeat thread");
        return;
    }

    LOG_INFO("Heartbeat thread started");
}

void stop_heartbeat_thread(void) {
    dsm_context_t *ctx = dsm_get_context();

    if (ctx->network.heartbeat_thread != 0) {
        /* running flag is already set to false by network_shutdown */
        pthread_join(ctx->network.heartbeat_thread, NULL);
        ctx->network.heartbeat_thread = 0;
        LOG_INFO("Heartbeat thread stopped");
    }
}

/* DIRECTORY PROTOCOL */
int send_dir_query(node_id_t manager, page_id_t page_id) {
    dsm_context_t *ctx = dsm_get_context();
    message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.header.magic = MSG_MAGIC;
    msg.header.type = MSG_DIR_QUERY;
    msg.header.sender = ctx->node_id;
    msg.payload.dir_query.page_id = page_id;
    msg.payload.dir_query.requester = ctx->node_id;

    LOG_INFO("Sending DIR_QUERY to node %u for page %lu", manager, page_id);
    int rc = network_send(manager, &msg);
    if (rc != DSM_SUCCESS) {
        LOG_ERROR("Failed to send DIR_QUERY to node %u (rc=%d)", manager, rc);
    }
    return rc;
}

int send_dir_reply(node_id_t requester, page_id_t page_id, node_id_t owner) {
    dsm_context_t *ctx = dsm_get_context();
    message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.header.magic = MSG_MAGIC;
    msg.header.type = MSG_DIR_REPLY;
    msg.header.sender = ctx->node_id;
    msg.payload.dir_reply.page_id = page_id;
    msg.payload.dir_reply.owner = owner;
    
    return network_send(requester, &msg);
}

int send_owner_update(node_id_t manager, page_id_t page_id, node_id_t new_owner) {
    dsm_context_t *ctx = dsm_get_context();
    message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.header.magic = MSG_MAGIC;
    msg.header.type = MSG_OWNER_UPDATE;
    msg.header.sender = ctx->node_id;
    msg.payload.owner_update.page_id = page_id;
    msg.payload.owner_update.new_owner = new_owner;
    
    return network_send(manager, &msg);
}

int handle_dir_query(const message_t *msg) {
    page_id_t page_id = msg->payload.dir_query.page_id;
    node_id_t requester = msg->payload.dir_query.requester;

    LOG_INFO("Received DIR_QUERY for page %lu from node %u", page_id, requester);

    page_directory_t *dir = get_page_directory();
    node_id_t owner = 0;

    if (dir) {
        int rc = directory_lookup(dir, page_id, &owner);
        LOG_INFO("Directory lookup for page %lu: owner=%u (rc=%d)", page_id, owner, rc);
    } else {
        LOG_WARN("No directory available for lookup");
    }

    LOG_INFO("Sending DIR_REPLY to node %u: page %lu owned by node %u", requester, page_id, owner);
    return send_dir_reply(requester, page_id, owner);
}

int handle_dir_reply(const message_t *msg) {
    page_id_t page_id = msg->payload.dir_reply.page_id;
    node_id_t owner = msg->payload.dir_reply.owner;

    LOG_INFO("Received DIR_REPLY for page %lu: owner=node %u", page_id, owner);

    dsm_context_t *ctx = dsm_get_context();
    pthread_mutex_lock(&ctx->network.dir_tracker.lock);
    
    if (ctx->network.dir_tracker.active && 
        ctx->network.dir_tracker.page_id == page_id) {
        ctx->network.dir_tracker.owner = owner;
        ctx->network.dir_tracker.complete = true;
        pthread_cond_signal(&ctx->network.dir_tracker.cv);
    }
    
    pthread_mutex_unlock(&ctx->network.dir_tracker.lock);
    return DSM_SUCCESS;
}

int handle_owner_update(const message_t *msg) {
    page_id_t page_id = msg->payload.owner_update.page_id;
    node_id_t new_owner = msg->payload.owner_update.new_owner;
    
    page_directory_t *dir = get_page_directory();
    if (dir) {
        directory_set_owner(dir, page_id, new_owner);
        LOG_DEBUG("Updated directory (via MSG): page %lu now owned by node %u", page_id, new_owner);
    }
    return DSM_SUCCESS;
}

int broadcast_node_failure(node_id_t failed_node) {
    dsm_context_t *ctx = dsm_get_context();
    
    /* Broadcast to all connected nodes */
    for (int i = 0; i < MAX_NODES; i++) {
        pthread_mutex_lock(&ctx->lock);
        bool connected = ctx->network.nodes[i].connected;
        node_id_t node_id = ctx->network.nodes[i].id;
        pthread_mutex_unlock(&ctx->lock);

        /* Don't send to self or the failed node */
        if (connected && node_id != ctx->node_id && node_id != failed_node) {
            
            message_t msg;
            memset(&msg, 0, sizeof(msg));
            msg.header.magic = MSG_MAGIC;
            msg.header.type = MSG_NODE_FAILED;
            msg.header.sender = ctx->node_id;
            msg.payload.node_failed.failed_node = failed_node;
            
            LOG_INFO("Broadcasting NODE_FAILED for node %u to node %u", 
                     failed_node, node_id);
            network_send(node_id, &msg);
        }
    }
    return DSM_SUCCESS;
}

int handle_node_failed_msg(const message_t *msg) {
    node_id_t failed_node = msg->payload.node_failed.failed_node;
    dsm_context_t *ctx = dsm_get_context();

    pthread_mutex_lock(&ctx->lock);
    if (failed_node < MAX_NODES && !ctx->network.nodes[failed_node].is_failed) {
         ctx->network.nodes[failed_node].is_failed = true;
         LOG_WARN("Marked node %u as FAILED via notification", failed_node);
    }
    pthread_mutex_unlock(&ctx->lock);

    page_directory_t *dir = get_page_directory();
    if (dir) {
        directory_handle_node_failure(dir, failed_node);
    }

    return DSM_SUCCESS;
}

/* CRITICAL FIX (BUG #8): Sharer tracking protocol */
int send_sharer_query(node_id_t owner, page_id_t page_id) {
    dsm_context_t *ctx = dsm_get_context();
    message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.header.magic = MSG_MAGIC;
    msg.header.type = MSG_SHARER_QUERY;
    msg.header.sender = ctx->node_id;
    msg.payload.sharer_query.page_id = page_id;
    msg.payload.sharer_query.requester = ctx->node_id;

    LOG_DEBUG("Querying node %u for sharers of page %lu", owner, page_id);
    return network_send(owner, &msg);
}

int handle_sharer_query(const message_t *msg) {
    page_id_t page_id = msg->payload.sharer_query.page_id;
    node_id_t requester = msg->payload.sharer_query.requester;

    LOG_DEBUG("Received SHARER_QUERY for page %lu from node %u", page_id, requester);

    /* Look up sharers in local directory */
    page_directory_t *dir = get_page_directory();
    node_id_t sharers[MAX_SHARERS];
    int num_sharers = 0;

    if (dir) {
        directory_get_sharers(dir, page_id, sharers, &num_sharers);
    }

    /* Send reply with sharer list */
    dsm_context_t *ctx = dsm_get_context();
    message_t reply;
    memset(&reply, 0, sizeof(reply));
    reply.header.magic = MSG_MAGIC;
    reply.header.type = MSG_SHARER_REPLY;
    reply.header.sender = ctx->node_id;
    reply.payload.sharer_reply.page_id = page_id;
    reply.payload.sharer_reply.num_sharers = num_sharers;
    for (int i = 0; i < num_sharers && i < MAX_SHARERS; i++) {
        reply.payload.sharer_reply.sharers[i] = sharers[i];
    }

    LOG_DEBUG("Sending SHARER_REPLY to node %u: page %lu has %d sharers",
             requester, page_id, num_sharers);
    return network_send(requester, &reply);
}

int handle_sharer_reply(const message_t *msg) {
    page_id_t page_id = msg->payload.sharer_reply.page_id;
    int num_sharers = msg->payload.sharer_reply.num_sharers;

    LOG_DEBUG("Received SHARER_REPLY for page %lu: %d sharers", page_id, num_sharers);

    /* Store in context for fetch_page_write to retrieve */
    dsm_context_t *ctx = dsm_get_context();
    pthread_mutex_lock(&ctx->network.sharer_tracker.lock);

    if (ctx->network.sharer_tracker.active &&
        ctx->network.sharer_tracker.page_id == page_id) {
        ctx->network.sharer_tracker.num_sharers = num_sharers;
        for (int i = 0; i < num_sharers && i < MAX_SHARERS; i++) {
            ctx->network.sharer_tracker.sharers[i] = msg->payload.sharer_reply.sharers[i];
        }
        ctx->network.sharer_tracker.complete = true;
        pthread_cond_signal(&ctx->network.sharer_tracker.cv);
        LOG_DEBUG("Stored sharer list for page %lu in tracker", page_id);
    }

    pthread_mutex_unlock(&ctx->network.sharer_tracker.lock);
    return DSM_SUCCESS;
}

/* Dispatcher */
int dispatch_message(const message_t *msg, int sockfd) {
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
        case MSG_ALLOC_NOTIFY:
            return handle_alloc_notify(msg);
        case MSG_ALLOC_ACK:
            return handle_alloc_ack(msg);
        case MSG_NODE_JOIN:
            return handle_node_join(msg, sockfd);
        case MSG_HEARTBEAT:
            return handle_heartbeat(msg);
        case MSG_HEARTBEAT_ACK:
            /* Optional, currently unused */
            return DSM_SUCCESS;
        case MSG_DIR_QUERY:
            return handle_dir_query(msg);
        case MSG_DIR_REPLY:
            return handle_dir_reply(msg);
        case MSG_OWNER_UPDATE:
            return handle_owner_update(msg);
        case MSG_NODE_FAILED:
            return handle_node_failed_msg(msg);
        case MSG_SHARER_QUERY:
            return handle_sharer_query(msg);
        case MSG_SHARER_REPLY:
            return handle_sharer_reply(msg);
        case MSG_NODE_LEAVE:
            LOG_INFO("Received NODE_LEAVE from node %u", msg->header.sender);
            return DSM_SUCCESS;
        case MSG_ERROR:
            {
                int error_code = msg->payload.error.error_code;
                page_id_t page_id = msg->payload.error.page_id;
                LOG_ERROR("Received ERROR message from node %u: code=%d, page=%lu, msg=%s",
                         msg->header.sender, error_code, page_id,
                         msg->payload.error.error_msg);

                /* If this error relates to a page request, we need to wake the waiter */
                dsm_context_t *ctx = dsm_get_context();
                
                /* Find relevant table */
                page_entry_t *entry = NULL;
                pthread_mutex_lock(&ctx->lock);
                for (int i = 0; i < ctx->num_allocations; i++) {
                    if (ctx->page_tables[i]) {
                        entry = page_table_lookup_by_id(ctx->page_tables[i], page_id);
                        if (entry) break;
                    }
                }
                pthread_mutex_unlock(&ctx->lock);
                
                if (entry) {
                    pthread_mutex_lock(&entry->entry_lock);
                    if (entry->request_pending) {
                        /* Propagate error code so fetch_page can retry */
                        entry->fetch_result = error_code;
                        entry->request_pending = false;
                        pthread_cond_broadcast(&entry->ready_cv);
                        LOG_DEBUG("Woke waiter for page %lu due to error", page_id);
                    }
                    pthread_mutex_unlock(&entry->entry_lock);
                }
                return DSM_SUCCESS;
            }
        default:
            LOG_WARN("Unknown message type: %d", msg->header.type);
            return DSM_ERROR_INVALID;
    }
}

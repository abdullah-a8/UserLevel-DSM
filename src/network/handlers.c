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

    return DSM_SUCCESS;
}

/* NODE_JOIN */
int send_node_join(node_id_t node_id, const char *hostname, uint16_t port) {
    dsm_context_t *ctx = dsm_get_context();
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
    /* Heartbeat received - connection is alive, nothing more to do */
    LOG_DEBUG("Received HEARTBEAT from node %u", msg->header.sender);
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
        case MSG_NODE_JOIN:
            return handle_node_join(msg, sockfd);
        case MSG_HEARTBEAT:
            return handle_heartbeat(msg);
        case MSG_NODE_LEAVE:
            LOG_INFO("Received NODE_LEAVE from node %u", msg->header.sender);
            return DSM_SUCCESS;
        case MSG_ERROR:
            LOG_ERROR("Received ERROR message from node %u: code=%d, msg=%s",
                     msg->header.sender,
                     msg->payload.error.error_code,
                     msg->payload.error.error_msg);
            return DSM_SUCCESS;
        default:
            LOG_WARN("Unknown message type: %d", msg->header.type);
            return DSM_ERROR_INVALID;
    }
}

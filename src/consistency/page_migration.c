/**
 * @file page_migration.c
 * @brief Page migration implementation
 */

#include "page_migration.h"
#include "directory.h"
#include "../core/log.h"
#include "../core/dsm_context.h"
#include "../core/perf_log.h"
#include "../memory/page_table.h"
#include "../memory/permission.h"
#include "../network/handlers.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Global page directory (managed by manager node or replicated) */
static page_directory_t *g_directory = NULL;

int consistency_init(size_t num_pages) {
    if (g_directory) {
        LOG_WARN("Consistency module already initialized");
        return DSM_SUCCESS;
    }

    g_directory = directory_create(num_pages);
    if (!g_directory) {
        LOG_ERROR("Failed to create page directory");
        return DSM_ERROR_MEMORY;
    }

    LOG_INFO("Consistency module initialized with %zu pages", num_pages);
    return DSM_SUCCESS;
}

void consistency_cleanup(void) {
    if (g_directory) {
        directory_destroy(g_directory);
        g_directory = NULL;
    }
}

page_directory_t* get_page_directory(void) {
    return g_directory;
}

int fetch_page_read(page_id_t page_id) {
    dsm_context_t *ctx = dsm_get_context();
    if (!ctx || !ctx->page_table || !g_directory) {
        LOG_ERROR("DSM not initialized");
        return DSM_ERROR_INIT;
    }

    /* Look up page entry */
    page_entry_t *entry = page_table_lookup_by_id(ctx->page_table, page_id);
    if (!entry) {
        LOG_ERROR("Page %lu not found in page table", page_id);
        return DSM_ERROR_NOT_FOUND;
    }

    /* Look up current owner in directory */
    node_id_t owner;
    int rc = directory_lookup(g_directory, page_id, &owner);
    if (rc != DSM_SUCCESS) {
        LOG_ERROR("Failed to lookup page %lu in directory", page_id);
        return rc;
    }

    LOG_DEBUG("Fetching page %lu for read from node %u", page_id, owner);

    /* If we are the owner, just upgrade permission */
    if (owner == ctx->node_id) {
        rc = set_page_permission(entry->local_addr, PAGE_PERM_READ);
        if (rc != DSM_SUCCESS) {
            return rc;
        }
        entry->state = PAGE_STATE_READ_ONLY;
        LOG_DEBUG("Page %lu already owned, upgraded to READ_ONLY", page_id);
        return DSM_SUCCESS;
    }

    /* Task 8.1: Request queuing to prevent thundering herd */
    pthread_mutex_lock(&entry->entry_lock);

    /* Check if another thread is already fetching this page */
    if (entry->request_pending) {
        /* Queue this request by incrementing waiting counter */
        entry->num_waiting_threads++;
        LOG_DEBUG("Page %lu fetch already in progress, queuing request (waiters=%d)",
                  page_id, entry->num_waiting_threads);

        /* Wait for the page to arrive (with timeout) */
        struct timespec timeout;
        clock_gettime(CLOCK_REALTIME, &timeout);
        timeout.tv_sec += 5;  /* 5 second timeout */

        while (entry->request_pending) {
            rc = pthread_cond_timedwait(&entry->ready_cv, &entry->entry_lock, &timeout);
            if (rc != 0) {
                LOG_ERROR("Timeout waiting for page %lu", page_id);
                entry->num_waiting_threads--;
                pthread_mutex_unlock(&entry->entry_lock);
                perf_log_timeout();  /* Task 8.6 */
                return DSM_ERROR_TIMEOUT;
            }
        }

        entry->num_waiting_threads--;
        pthread_mutex_unlock(&entry->entry_lock);

        /* Page should now be available */
        LOG_DEBUG("Page %lu now available after queued wait", page_id);
        return DSM_SUCCESS;
    }

    /* This thread will fetch the page */
    entry->request_pending = true;
    pthread_mutex_unlock(&entry->entry_lock);

    /* Send PAGE_REQUEST with READ access */
    rc = send_page_request(owner, page_id, ACCESS_READ);
    if (rc != DSM_SUCCESS) {
        LOG_ERROR("Failed to send PAGE_REQUEST to node %u", owner);
        pthread_mutex_lock(&entry->entry_lock);
        entry->request_pending = false;
        pthread_cond_broadcast(&entry->ready_cv);  /* Wake any waiters */
        pthread_mutex_unlock(&entry->entry_lock);
        return rc;
    }

    /* Wait for PAGE_REPLY (with timeout) */
    pthread_mutex_lock(&entry->entry_lock);
    struct timespec timeout;
    clock_gettime(CLOCK_REALTIME, &timeout);
    timeout.tv_sec += 5;  /* 5 second timeout (Task 8.2) */

    while (entry->request_pending) {
        rc = pthread_cond_timedwait(&entry->ready_cv, &entry->entry_lock, &timeout);
        if (rc != 0) {
            LOG_ERROR("Timeout waiting for page %lu", page_id);
            entry->request_pending = false;
            pthread_cond_broadcast(&entry->ready_cv);  /* Wake any waiters */
            pthread_mutex_unlock(&entry->entry_lock);
            perf_log_timeout();  /* Task 8.6 */
            return DSM_ERROR_TIMEOUT;
        }
    }
    pthread_mutex_unlock(&entry->entry_lock);

    /* Update directory: add self as sharer */
    directory_add_reader(g_directory, page_id, ctx->node_id);

    /* Update local state */
    entry->state = PAGE_STATE_READ_ONLY;

    /* Update stats */
    pthread_mutex_lock(&ctx->stats_lock);
    ctx->stats.pages_fetched++;
    pthread_mutex_unlock(&ctx->stats_lock);

    LOG_DEBUG("Successfully fetched page %lu for read", page_id);
    return DSM_SUCCESS;
}

int fetch_page_write(page_id_t page_id) {
    dsm_context_t *ctx = dsm_get_context();
    if (!ctx || !ctx->page_table || !g_directory) {
        LOG_ERROR("DSM not initialized");
        return DSM_ERROR_INIT;
    }

    /* Look up page entry */
    page_entry_t *entry = page_table_lookup_by_id(ctx->page_table, page_id);
    if (!entry) {
        LOG_ERROR("Page %lu not found in page table", page_id);
        return DSM_ERROR_NOT_FOUND;
    }

    /* Look up current owner and get invalidation list */
    node_id_t owner;
    int rc = directory_lookup(g_directory, page_id, &owner);
    if (rc != DSM_SUCCESS) {
        LOG_ERROR("Failed to lookup page %lu in directory", page_id);
        return rc;
    }

    LOG_DEBUG("Fetching page %lu for write from node %u", page_id, owner);

    /* Task 8.1: Request queuing for write requests */
    pthread_mutex_lock(&entry->entry_lock);

    /* Check if another thread is already fetching this page for write */
    if (entry->request_pending) {
        /* Queue this request by incrementing waiting counter */
        entry->num_waiting_threads++;
        LOG_DEBUG("Page %lu write fetch already in progress, queuing request (waiters=%d)",
                  page_id, entry->num_waiting_threads);

        /* Wait for the page to arrive (with timeout) */
        struct timespec timeout;
        clock_gettime(CLOCK_REALTIME, &timeout);
        timeout.tv_sec += 10;  /* 10 second timeout for writes (longer than reads) */

        while (entry->request_pending) {
            rc = pthread_cond_timedwait(&entry->ready_cv, &entry->entry_lock, &timeout);
            if (rc != 0) {
                LOG_ERROR("Timeout waiting for page %lu (write)", page_id);
                entry->num_waiting_threads--;
                pthread_mutex_unlock(&entry->entry_lock);
                perf_log_timeout();  /* Task 8.6 */
                return DSM_ERROR_TIMEOUT;
            }
        }

        entry->num_waiting_threads--;
        pthread_mutex_unlock(&entry->entry_lock);

        /* Page should now be available */
        LOG_DEBUG("Page %lu now available after queued write wait", page_id);
        return DSM_SUCCESS;
    }

    /* This thread will fetch the page */
    entry->request_pending = true;
    pthread_mutex_unlock(&entry->entry_lock);

    /* Get list of nodes to invalidate */
    node_id_t invalidate_list[MAX_SHARERS];
    int num_invalidate = 0;
    rc = directory_set_writer(g_directory, page_id, ctx->node_id,
                              invalidate_list, &num_invalidate);
    if (rc != DSM_SUCCESS) {
        LOG_ERROR("Failed to update directory for page %lu", page_id);
        pthread_mutex_lock(&entry->entry_lock);
        entry->request_pending = false;
        pthread_cond_broadcast(&entry->ready_cv);
        pthread_mutex_unlock(&entry->entry_lock);
        return rc;
    }

    /* Send invalidations to all sharers */
    for (int i = 0; i < num_invalidate; i++) {
        LOG_DEBUG("Sending invalidation for page %lu to node %u",
                  page_id, invalidate_list[i]);
        rc = send_invalidate(invalidate_list[i], page_id);
        if (rc != DSM_SUCCESS) {
            LOG_WARN("Failed to send invalidation to node %u", invalidate_list[i]);
        }

        /* Update stats */
        pthread_mutex_lock(&ctx->stats_lock);
        ctx->stats.invalidations_sent++;
        pthread_mutex_unlock(&ctx->stats_lock);
    }

    /* Wait for ACKs (simplified - in real implementation would track ACKs) */
    /* For now, just sleep briefly to allow ACKs to arrive */
    if (num_invalidate > 0) {
        struct timespec wait = {0, 100000000};  /* 100ms */
        nanosleep(&wait, NULL);
    }

    /* If we were not the owner, request page data */
    if (owner != ctx->node_id) {
        /* Send PAGE_REQUEST with WRITE access */
        rc = send_page_request(owner, page_id, ACCESS_WRITE);
        if (rc != DSM_SUCCESS) {
            LOG_ERROR("Failed to send PAGE_REQUEST to node %u", owner);
            pthread_mutex_lock(&entry->entry_lock);
            entry->request_pending = false;
            pthread_cond_broadcast(&entry->ready_cv);
            pthread_mutex_unlock(&entry->entry_lock);
            return rc;
        }

        /* Wait for PAGE_REPLY (with timeout) */
        pthread_mutex_lock(&entry->entry_lock);
        struct timespec timeout;
        clock_gettime(CLOCK_REALTIME, &timeout);
        timeout.tv_sec += 10;  /* 10 second timeout (Task 8.2) */

        while (entry->request_pending) {
            rc = pthread_cond_timedwait(&entry->ready_cv, &entry->entry_lock, &timeout);
            if (rc != 0) {
                LOG_ERROR("Timeout waiting for page %lu", page_id);
                entry->request_pending = false;
                pthread_cond_broadcast(&entry->ready_cv);
                pthread_mutex_unlock(&entry->entry_lock);
                perf_log_timeout();  /* Task 8.6 */
                return DSM_ERROR_TIMEOUT;
            }
        }
        pthread_mutex_unlock(&entry->entry_lock);

        /* Update stats */
        pthread_mutex_lock(&ctx->stats_lock);
        ctx->stats.pages_fetched++;
        pthread_mutex_unlock(&ctx->stats_lock);
    } else {
        /* We already own it, just upgrade permission */
        rc = set_page_permission(entry->local_addr, PAGE_PERM_READ_WRITE);
        if (rc != DSM_SUCCESS) {
            pthread_mutex_lock(&entry->entry_lock);
            entry->request_pending = false;
            pthread_cond_broadcast(&entry->ready_cv);
            pthread_mutex_unlock(&entry->entry_lock);
            return rc;
        }

        /* Clear request pending flag */
        pthread_mutex_lock(&entry->entry_lock);
        entry->request_pending = false;
        pthread_cond_broadcast(&entry->ready_cv);
        pthread_mutex_unlock(&entry->entry_lock);
    }

    /* Update local state */
    entry->state = PAGE_STATE_READ_WRITE;
    entry->owner = ctx->node_id;

    LOG_DEBUG("Successfully fetched page %lu for write", page_id);
    return DSM_SUCCESS;
}

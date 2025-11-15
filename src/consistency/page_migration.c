/**
 * @file page_migration.c
 * @brief Page migration implementation
 */

#include "page_migration.h"
#include "directory.h"
#include "../core/log.h"
#include "../core/dsm_context.h"
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

    /* Mark request as pending */
    pthread_mutex_lock(&ctx->page_table->lock);
    entry->request_pending = true;
    pthread_mutex_unlock(&ctx->page_table->lock);

    /* Send PAGE_REQUEST with READ access */
    rc = send_page_request(owner, page_id, ACCESS_READ);
    if (rc != DSM_SUCCESS) {
        LOG_ERROR("Failed to send PAGE_REQUEST to node %u", owner);
        pthread_mutex_lock(&ctx->page_table->lock);
        entry->request_pending = false;
        pthread_mutex_unlock(&ctx->page_table->lock);
        return rc;
    }

    /* Wait for PAGE_REPLY (with timeout) */
    pthread_mutex_lock(&ctx->page_table->lock);
    struct timespec timeout;
    clock_gettime(CLOCK_REALTIME, &timeout);
    timeout.tv_sec += 5;  /* 5 second timeout */

    while (entry->request_pending) {
        rc = pthread_cond_timedwait(&entry->ready_cv, &ctx->page_table->lock, &timeout);
        if (rc != 0) {
            LOG_ERROR("Timeout waiting for page %lu", page_id);
            entry->request_pending = false;
            pthread_mutex_unlock(&ctx->page_table->lock);
            return DSM_ERROR_TIMEOUT;
        }
    }
    pthread_mutex_unlock(&ctx->page_table->lock);

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

    /* Get list of nodes to invalidate */
    node_id_t invalidate_list[MAX_SHARERS];
    int num_invalidate = 0;
    rc = directory_set_writer(g_directory, page_id, ctx->node_id,
                              invalidate_list, &num_invalidate);
    if (rc != DSM_SUCCESS) {
        LOG_ERROR("Failed to update directory for page %lu", page_id);
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
        /* Mark request as pending */
        pthread_mutex_lock(&ctx->page_table->lock);
        entry->request_pending = true;
        pthread_mutex_unlock(&ctx->page_table->lock);

        /* Send PAGE_REQUEST with WRITE access */
        rc = send_page_request(owner, page_id, ACCESS_WRITE);
        if (rc != DSM_SUCCESS) {
            LOG_ERROR("Failed to send PAGE_REQUEST to node %u", owner);
            pthread_mutex_lock(&ctx->page_table->lock);
            entry->request_pending = false;
            pthread_mutex_unlock(&ctx->page_table->lock);
            return rc;
        }

        /* Wait for PAGE_REPLY (with timeout) */
        pthread_mutex_lock(&ctx->page_table->lock);
        struct timespec timeout;
        clock_gettime(CLOCK_REALTIME, &timeout);
        timeout.tv_sec += 5;  /* 5 second timeout */

        while (entry->request_pending) {
            rc = pthread_cond_timedwait(&entry->ready_cv, &ctx->page_table->lock, &timeout);
            if (rc != 0) {
                LOG_ERROR("Timeout waiting for page %lu", page_id);
                entry->request_pending = false;
                pthread_mutex_unlock(&ctx->page_table->lock);
                return DSM_ERROR_TIMEOUT;
            }
        }
        pthread_mutex_unlock(&ctx->page_table->lock);

        /* Update stats */
        pthread_mutex_lock(&ctx->stats_lock);
        ctx->stats.pages_fetched++;
        pthread_mutex_unlock(&ctx->stats_lock);
    } else {
        /* We already own it, just upgrade permission */
        rc = set_page_permission(entry->local_addr, PAGE_PERM_READ_WRITE);
        if (rc != DSM_SUCCESS) {
            return rc;
        }
    }

    /* Update local state */
    entry->state = PAGE_STATE_READ_WRITE;
    entry->owner = ctx->node_id;

    LOG_DEBUG("Successfully fetched page %lu for write", page_id);
    return DSM_SUCCESS;
}

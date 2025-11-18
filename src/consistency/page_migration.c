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
#include <errno.h>
#include <unistd.h>

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
    int final_result = DSM_SUCCESS;

    if (!ctx || ctx->num_allocations == 0 || !g_directory) {
        LOG_ERROR("DSM not initialized");
        return DSM_ERROR_INIT;
    }

    /* Search all page tables for this page ID */
    page_entry_t *entry = NULL;
    page_table_t *owning_table = NULL;

    pthread_mutex_lock(&ctx->lock);
    for (int i = 0; i < ctx->num_allocations; i++) {
        if (ctx->page_tables[i]) {
            entry = page_table_lookup_by_id(ctx->page_tables[i], page_id);
            if (entry) {
                owning_table = ctx->page_tables[i];
                page_table_acquire(owning_table); /* Protected: Prevent use-after-free */
                break;
            }
        }
    }
    pthread_mutex_unlock(&ctx->lock);

    if (!entry) {
        LOG_ERROR("Page %lu not found in any page table", page_id);
        return DSM_ERROR_NOT_FOUND;
    }

    int retries = 0;
    const int MAX_RETRIES = 3;

    while (retries < MAX_RETRIES) {
        /* Look up current owner in directory */
        node_id_t owner;
        int rc = query_directory_manager(page_id, &owner);
        if (rc != DSM_SUCCESS) {
            LOG_ERROR("Failed to lookup page %lu in directory", page_id);
            final_result = rc;
            goto cleanup;
        }

        LOG_DEBUG("Fetching page %lu for read from node %u (attempt %d/%d)", 
                  page_id, owner, retries + 1, MAX_RETRIES);

        /* If we are the owner, just upgrade permission */
        if (owner == ctx->node_id) {
            rc = set_page_permission(entry->local_addr, PAGE_PERM_READ);
            if (rc != DSM_SUCCESS) {
                final_result = rc;
                goto cleanup;
            }
            entry->state = PAGE_STATE_READ_ONLY;
            LOG_DEBUG("Page %lu already owned, upgraded to READ_ONLY", page_id);
            final_result = DSM_SUCCESS;
            goto cleanup;
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
                    LOG_ERROR("Timeout waiting for page %lu (queued thread)", page_id);
                    entry->num_waiting_threads--;
                    pthread_mutex_unlock(&entry->entry_lock);
                    perf_log_timeout();  /* Task 8.6 */
                    final_result = DSM_ERROR_TIMEOUT;
                    goto cleanup;
                }
            }

            entry->num_waiting_threads--;

            /* CRITICAL FIX (BUG 1): Check fetch result before returning */
            int result = entry->fetch_result;
            pthread_mutex_unlock(&entry->entry_lock);

            if (result != DSM_SUCCESS) {
                LOG_WARN("Page %lu fetch failed in primary thread (result=%d), retrying...", page_id, result);
                /* If primary failed, we retry the whole loop */
                retries++;
                usleep(100000 * retries); /* Backoff */
                continue;
            }

            LOG_DEBUG("Page %lu now available after queued wait", page_id);
            final_result = DSM_SUCCESS;
            goto cleanup;
        }

        /* This thread will fetch the page */
        entry->request_pending = true;
        pthread_mutex_unlock(&entry->entry_lock);

        /* Send PAGE_REQUEST with READ access */
        rc = send_page_request(owner, page_id, ACCESS_READ);
        if (rc != DSM_SUCCESS) {
            LOG_ERROR("Failed to send PAGE_REQUEST to node %u", owner);
            pthread_mutex_lock(&entry->entry_lock);
            entry->fetch_result = rc;  /* BUG FIX: Set result before waking waiters */
            entry->request_pending = false;
            pthread_cond_broadcast(&entry->ready_cv);  /* Wake any waiters */
            pthread_mutex_unlock(&entry->entry_lock);
            
            retries++;
            usleep(100000 * retries);
            continue;
        }

        /* Wait for PAGE_REPLY (with timeout) */
        pthread_mutex_lock(&entry->entry_lock);
        struct timespec timeout;
        clock_gettime(CLOCK_REALTIME, &timeout);
        timeout.tv_sec += 5;  /* 5 second timeout (Task 8.2) */

        while (entry->request_pending) {
            rc = pthread_cond_timedwait(&entry->ready_cv, &entry->entry_lock, &timeout);
            if (rc != 0) {
                LOG_ERROR("Timeout waiting for page %lu from node %u", page_id, owner);

                /* CRITICAL FIX: Check if owner has failed and attempt recovery */
                bool owner_failed = false;
                pthread_mutex_lock(&ctx->lock);
                if (owner < MAX_NODES && ctx->network.nodes[owner].is_failed) {
                    owner_failed = true;
                }
                pthread_mutex_unlock(&ctx->lock);

                if (owner_failed) {
                    LOG_WARN("Page %lu owner (node %u) has failed - reclaiming ownership for READ",
                             page_id, owner);

                    /* Reclaim ownership in directory */
                    directory_reclaim_ownership(g_directory, page_id, ctx->node_id);

                    /* Initialize page with zeros (data lost from failed node) */
                    memset(entry->local_addr, 0, PAGE_SIZE);

                    /* Set permission to READ */
                    rc = set_page_permission(entry->local_addr, PAGE_PERM_READ);
                    if (rc != DSM_SUCCESS) {
                        LOG_ERROR("Failed to set page permission after recovery");
                        entry->fetch_result = rc;
                        entry->request_pending = false;
                        pthread_cond_broadcast(&entry->ready_cv);
                        pthread_mutex_unlock(&entry->entry_lock);
                        final_result = rc;
                        goto cleanup;
                    }

                    entry->state = PAGE_STATE_READ_ONLY;
                    entry->owner = ctx->node_id;

                    entry->fetch_result = DSM_SUCCESS;
                    entry->request_pending = false;
                    pthread_cond_broadcast(&entry->ready_cv);
                    pthread_mutex_unlock(&entry->entry_lock);

                    LOG_INFO("Successfully recovered page %lu from failed node %u", page_id, owner);
                    final_result = DSM_SUCCESS;  /* Recovery successful */
                    goto cleanup;
                }

                /* If it was a timeout but owner not failed, or MSG_ERROR received (fetch_result set) */
                /* If request_pending is still true, it's a timeout. If false, it was likely an error signal */
                
                /* We assume timeout for now if we are here and rc != 0 */
                entry->fetch_result = DSM_ERROR_TIMEOUT;
                entry->request_pending = false;
                pthread_cond_broadcast(&entry->ready_cv);  /* Wake any waiters with error */
                pthread_mutex_unlock(&entry->entry_lock);
                perf_log_timeout();  /* Task 8.6 */

                /* Retry loop will catch this */
                break; 
            }
        }
        
        /* Check result */
        int result = entry->fetch_result;
        if (!entry->request_pending) {
             /* Loop finished (either success or error signaled) */
             pthread_mutex_unlock(&entry->entry_lock);
             
             if (result == DSM_SUCCESS) {
                 /* Success path */
                 rc = directory_add_reader(g_directory, page_id, ctx->node_id);
                 if (rc != DSM_SUCCESS && rc != DSM_ERROR_BUSY) {
                     LOG_ERROR("Failed to add reader to directory for page %lu", page_id);
                     final_result = rc;
                     goto cleanup;
                 }
                 
                 entry->state = PAGE_STATE_READ_ONLY;
                 pthread_mutex_lock(&ctx->stats_lock);
                 ctx->stats.pages_fetched++;
                 pthread_mutex_unlock(&ctx->stats_lock);
                 
                 LOG_DEBUG("Successfully fetched page %lu for read", page_id);
                 final_result = DSM_SUCCESS;
                 goto cleanup;
             } else {
                 /* Error path (including DSM_ERROR_INVALID from stale owner) */
                 LOG_WARN("Fetch failed with code %d, retrying...", result);
                 retries++;
                 usleep(100000 * retries);
                 continue;
             }
        } else {
            /* Should not happen if logic above is correct (lock held) */
            pthread_mutex_unlock(&entry->entry_lock);
        }
    }

    LOG_ERROR("Failed to fetch page %lu after %d retries", page_id, MAX_RETRIES);
    final_result = DSM_ERROR_TIMEOUT;

cleanup:
    if (owning_table) {
        page_table_release(owning_table);
    }
    return final_result;
}

int fetch_page_write(page_id_t page_id) {
    dsm_context_t *ctx = dsm_get_context();
    int final_result = DSM_SUCCESS;

    if (!ctx || ctx->num_allocations == 0 || !g_directory) {
        LOG_ERROR("DSM not initialized");
        return DSM_ERROR_INIT;
    }

    /* Search all page tables for this page ID */
    page_entry_t *entry = NULL;
    page_table_t *owning_table = NULL;

    pthread_mutex_lock(&ctx->lock);
    for (int i = 0; i < ctx->num_allocations; i++) {
        if (ctx->page_tables[i]) {
            entry = page_table_lookup_by_id(ctx->page_tables[i], page_id);
            if (entry) {
                owning_table = ctx->page_tables[i];
                page_table_acquire(owning_table); /* Protected: Prevent use-after-free */
                break;
            }
        }
    }
    pthread_mutex_unlock(&ctx->lock);

    if (!entry) {
        LOG_ERROR("Page %lu not found in any page table", page_id);
        return DSM_ERROR_NOT_FOUND;
    }

    int retries = 0;
    const int MAX_RETRIES = 3;

    while (retries < MAX_RETRIES) {
        /* Look up current owner and get invalidation list */
        node_id_t owner;
        int rc = query_directory_manager(page_id, &owner);
        if (rc != DSM_SUCCESS) {
            LOG_ERROR("Failed to lookup page %lu in directory", page_id);
            final_result = rc;
            goto cleanup;
        }

        LOG_DEBUG("Fetching page %lu for write from node %u (attempt %d/%d)", 
                  page_id, owner, retries + 1, MAX_RETRIES);

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
                    LOG_ERROR("Timeout waiting for page %lu (write, queued thread)", page_id);
                    entry->num_waiting_threads--;
                    pthread_mutex_unlock(&entry->entry_lock);
                    perf_log_timeout();  /* Task 8.6 */
                    final_result = DSM_ERROR_TIMEOUT;
                    goto cleanup;
                }
            }

            entry->num_waiting_threads--;

            /* CRITICAL FIX (BUG 2): Check fetch result before returning */
            int result = entry->fetch_result;
            pthread_mutex_unlock(&entry->entry_lock);

            if (result != DSM_SUCCESS) {
                LOG_WARN("Page %lu write fetch failed in primary thread (result=%d), retrying...", page_id, result);
                /* If primary failed, we retry the whole loop */
                retries++;
                usleep(100000 * retries);
                continue;
            }

            LOG_DEBUG("Page %lu now available after queued write wait", page_id);
            final_result = DSM_SUCCESS;
            goto cleanup;
        }

        /* This thread will fetch the page */
        entry->request_pending = true;
        pthread_mutex_unlock(&entry->entry_lock);

        /* PERFORMANCE NOTE: Get list of nodes to invalidate and set ourselves as owner */
        node_id_t invalidate_list[MAX_SHARERS];
        int num_invalidate = 0;
        rc = directory_set_writer(g_directory, page_id, ctx->node_id,
                                  invalidate_list, &num_invalidate);
        if (rc != DSM_SUCCESS) {
            LOG_ERROR("Failed to update directory for page %lu", page_id);
            pthread_mutex_lock(&entry->entry_lock);
            entry->fetch_result = rc;  /* BUG FIX: Set result before waking waiters */
            entry->request_pending = false;
            pthread_cond_broadcast(&entry->ready_cv);
            pthread_mutex_unlock(&entry->entry_lock);
            
            /* Directory failure usually isn't transient if local, but if we retry query_manager... */
            /* This specific error is local directory failure */
            final_result = rc;
            goto cleanup;
        }

        /* Initialize ACK counter before sending invalidations */
        pthread_mutex_lock(&entry->entry_lock);
        entry->pending_inv_acks = num_invalidate;
        pthread_mutex_unlock(&entry->entry_lock);

        /* CRITICAL FIX #3: Send invalidations to all sharers (skip failed nodes) */
        for (int i = 0; i < num_invalidate; i++) {
            node_id_t target = invalidate_list[i];

            /* Skip failed nodes */
            bool is_failed = false;
            pthread_mutex_lock(&ctx->lock);
            if (target < MAX_NODES && ctx->network.nodes[target].is_failed) {
                is_failed = true;
                LOG_WARN("Skipping invalidation to failed node %u for page %lu",
                         target, page_id);
            }
            pthread_mutex_unlock(&ctx->lock);

            if (is_failed) {
                /* Decrement pending count for failed node */
                pthread_mutex_lock(&entry->entry_lock);
                entry->pending_inv_acks--;
                pthread_mutex_unlock(&entry->entry_lock);
                continue;
            }

            LOG_DEBUG("Sending invalidation for page %lu to node %u",
                      page_id, target);
            rc = send_invalidate(target, page_id);
            if (rc != DSM_SUCCESS) {
                LOG_WARN("Failed to send invalidation to node %u", target);
                /* Decrement pending count if send failed */
                pthread_mutex_lock(&entry->entry_lock);
                entry->pending_inv_acks--;
                pthread_mutex_unlock(&entry->entry_lock);
            }

            /* Update stats */
            pthread_mutex_lock(&ctx->stats_lock);
            ctx->stats.invalidations_sent++;
            pthread_mutex_unlock(&ctx->stats_lock);
        }

        /* Wait for all invalidation ACKs with timeout */
        if (num_invalidate > 0) {
            pthread_mutex_lock(&entry->entry_lock);
            struct timespec timeout;
            clock_gettime(CLOCK_REALTIME, &timeout);
            timeout.tv_sec += 5;  /* 5 second timeout for ACKs */

            while (entry->pending_inv_acks > 0) {
                rc = pthread_cond_timedwait(&entry->inv_ack_cv, &entry->entry_lock, &timeout);
                if (rc == ETIMEDOUT) {
                    LOG_WARN("Timeout waiting for %d invalidation ACKs for page %lu",
                             entry->pending_inv_acks, page_id);
                    entry->pending_inv_acks = 0;  /* Reset to avoid blocking forever */
                    break;
                }
            }
            pthread_mutex_unlock(&entry->entry_lock);
            LOG_DEBUG("Received all invalidation ACKs for page %lu", page_id);
        }

        /* All invalidations complete - now safe to clear sharers */
        directory_clear_sharers(g_directory, page_id);

        /* If we were not the owner, request page data */
        if (owner != ctx->node_id) {
            /* Send PAGE_REQUEST with WRITE access */
            rc = send_page_request(owner, page_id, ACCESS_WRITE);
            if (rc != DSM_SUCCESS) {
                LOG_ERROR("Failed to send PAGE_REQUEST to node %u", owner);
                pthread_mutex_lock(&entry->entry_lock);
                entry->fetch_result = rc;  /* BUG FIX: Set result before waking waiters */
                entry->request_pending = false;
                pthread_cond_broadcast(&entry->ready_cv);
                pthread_mutex_unlock(&entry->entry_lock);
                
                retries++;
                usleep(100000 * retries);
                continue;
            }

            /* Wait for PAGE_REPLY (with timeout) */
            pthread_mutex_lock(&entry->entry_lock);
            struct timespec timeout;
            clock_gettime(CLOCK_REALTIME, &timeout);
            timeout.tv_sec += 10;  /* 10 second timeout (Task 8.2) */

            while (entry->request_pending) {
                rc = pthread_cond_timedwait(&entry->ready_cv, &entry->entry_lock, &timeout);
                if (rc != 0) {
                    LOG_ERROR("Timeout waiting for page %lu from node %u (WRITE)", page_id, owner);

                    /* CRITICAL FIX: Check if owner has failed and attempt recovery */
                    bool owner_failed = false;
                    pthread_mutex_lock(&ctx->lock);
                    if (owner < MAX_NODES && ctx->network.nodes[owner].is_failed) {
                        owner_failed = true;
                    }
                    pthread_mutex_unlock(&ctx->lock);

                    if (owner_failed) {
                        LOG_WARN("Page %lu owner (node %u) has failed - reclaiming ownership for WRITE",
                                 page_id, owner);

                        /* Reclaim ownership in directory */
                        directory_reclaim_ownership(g_directory, page_id, ctx->node_id);

                        /* Initialize page with zeros (data lost from failed node) */
                        memset(entry->local_addr, 0, PAGE_SIZE);

                        /* Set permission to READ_WRITE */
                        rc = set_page_permission(entry->local_addr, PAGE_PERM_READ_WRITE);
                        if (rc != DSM_SUCCESS) {
                            LOG_ERROR("Failed to set page permission after recovery");
                            entry->fetch_result = rc;
                            entry->request_pending = false;
                            pthread_cond_broadcast(&entry->ready_cv);
                            pthread_mutex_unlock(&entry->entry_lock);
                            final_result = rc;
                            goto cleanup;
                        }

                        entry->state = PAGE_STATE_READ_WRITE;
                        entry->owner = ctx->node_id;

                        entry->fetch_result = DSM_SUCCESS;
                        entry->request_pending = false;
                        pthread_cond_broadcast(&entry->ready_cv);
                        pthread_mutex_unlock(&entry->entry_lock);

                        LOG_INFO("Successfully recovered page %lu from failed node %u (WRITE)",
                                 page_id, owner);
                        final_result = DSM_SUCCESS;  /* Recovery successful */
                        goto cleanup;
                    }

                    /* Check if error or timeout */
                    entry->fetch_result = DSM_ERROR_TIMEOUT;
                    entry->request_pending = false;
                    pthread_cond_broadcast(&entry->ready_cv);  /* Wake any waiters with error */
                    pthread_mutex_unlock(&entry->entry_lock);
                    perf_log_timeout();  /* Task 8.6 */

                    /* Retry loop catches this */
                    break; 
                }
            }
            
            /* Check result */
            int result = entry->fetch_result;
            if (!entry->request_pending) {
                 pthread_mutex_unlock(&entry->entry_lock);
                 
                 if (result == DSM_SUCCESS) {
                     /* Success - Update stats */
                     pthread_mutex_lock(&ctx->stats_lock);
                     ctx->stats.pages_fetched++;
                     pthread_mutex_unlock(&ctx->stats_lock);
                 } else {
                     LOG_WARN("Fetch write failed with code %d, retrying...", result);
                     retries++;
                     usleep(100000 * retries);
                     continue;
                 }
            } else {
                pthread_mutex_unlock(&entry->entry_lock);
            }
        } else {
            /* We already own it, just upgrade permission */
            rc = set_page_permission(entry->local_addr, PAGE_PERM_READ_WRITE);
            if (rc != DSM_SUCCESS) {
                pthread_mutex_lock(&entry->entry_lock);
                entry->fetch_result = rc;  /* BUG FIX: Set error result before waking waiters */
                entry->request_pending = false;
                pthread_cond_broadcast(&entry->ready_cv);
                pthread_mutex_unlock(&entry->entry_lock);
                final_result = rc;
                goto cleanup;
            }

            /* Clear request pending flag with success result */
            pthread_mutex_lock(&entry->entry_lock);
            entry->fetch_result = DSM_SUCCESS;  /* BUG FIX: Set success result */
            entry->request_pending = false;
            pthread_cond_broadcast(&entry->ready_cv);
            pthread_mutex_unlock(&entry->entry_lock);
        }

        /* Update local state */
        entry->state = PAGE_STATE_READ_WRITE;
        entry->owner = ctx->node_id;

        /* Send OWNER_UPDATE to manager */
        if (ctx->node_id != 0) {
            send_owner_update(0, page_id, ctx->node_id);
        }

        LOG_DEBUG("Successfully fetched page %lu for write", page_id);
        final_result = DSM_SUCCESS;
        goto cleanup;
    }

    LOG_ERROR("Failed to fetch page %lu after %d retries", page_id, MAX_RETRIES);
    final_result = DSM_ERROR_TIMEOUT;

cleanup:
    if (owning_table) {
        page_table_release(owning_table);
    }
    return final_result;
}

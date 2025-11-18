/**
 * @file allocator.c
 * @brief DSM memory allocator
 */

#include "dsm/dsm.h"
#include "../core/dsm_context.h"
#include "../core/log.h"
#include "page_table.h"
#include "../consistency/page_migration.h"
#include "../consistency/directory.h"
#include "../network/handlers.h"
#include <sys/mman.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void* dsm_malloc(size_t size) {
    if (size == 0) {
        LOG_ERROR("dsm_malloc: size is 0");
        return NULL;
    }

    dsm_context_t *ctx = dsm_get_context();
    if (!ctx->initialized) {
        LOG_ERROR("DSM not initialized");
        return NULL;
    }

    /* Round up to page boundary */
    size_t aligned_size = ((size + PAGE_SIZE - 1) / PAGE_SIZE) * PAGE_SIZE;
    size_t num_pages = aligned_size / PAGE_SIZE;

    LOG_INFO("dsm_malloc: size=%zu, aligned=%zu, pages=%zu",
             size, aligned_size, num_pages);

    /* Allocate using mmap with PROT_NONE (no access initially) */
    void *addr = mmap(NULL, aligned_size, PROT_NONE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (addr == MAP_FAILED) {
        LOG_ERROR("mmap failed for size %zu", aligned_size);
        return NULL;
    }

    /* Create page table for this allocation */
    pthread_mutex_lock(&ctx->lock);

    if (ctx->num_allocations >= 32) {
        pthread_mutex_unlock(&ctx->lock);
        munmap(addr, aligned_size);
        LOG_ERROR("Maximum number of allocations (32) exceeded");
        return NULL;
    }

    /* CRITICAL FIX: Use num_local_allocations for allocation_index to prevent
     * remote allocations from corrupting local page ID assignment */
    page_table_t *new_table = page_table_create(addr, aligned_size, ctx->node_id, ctx->num_local_allocations);
    if (!new_table) {
        pthread_mutex_unlock(&ctx->lock);
        munmap(addr, aligned_size);
        LOG_ERROR("Failed to create page table");
        return NULL;
    }

    /* Add to list of page tables */
    ctx->page_tables[ctx->num_allocations] = new_table;
    ctx->num_allocations++;
    ctx->num_local_allocations++;  /* CRITICAL FIX: Increment local allocation counter */

    /* Set primary page table for backward compatibility */
    if (ctx->page_table == NULL) {
        ctx->page_table = new_table;

        /* Initialize consistency module now that first page table exists */
        /* Note: We use a hash table with 100K buckets for the page directory.
         * This provides good performance while using minimal memory (~8 MB for buckets).
         * Entries are created on-demand when pages are accessed, so actual memory usage
         * depends on the number of pages in use, not the maximum possible page ID.
         *
         * Memory usage: 100K buckets * 8 bytes/pointer = 800 KB
         * Plus: ~128 bytes per actual page entry (allocated on demand)
         * Example: 10K pages in use = 800 KB + 10K * 128 bytes = ~2 MB total
         *
         * This is a huge improvement over the previous 1.9 GB fixed allocation! */
        int rc = consistency_init(100000);
        if (rc != DSM_SUCCESS && rc != DSM_ERROR_INIT) {
            LOG_ERROR("Failed to initialize consistency module");
            page_table_destroy(new_table);
            ctx->page_tables[ctx->num_allocations - 1] = NULL;
            ctx->num_allocations--;
            ctx->page_table = NULL;
            pthread_mutex_unlock(&ctx->lock);
            munmap(addr, aligned_size);
            return NULL;
        }
    }

    /* Set this node as owner of all allocated pages */
    struct page_directory_s *dir = get_page_directory();
    if (dir) {
        for (size_t i = 0; i < num_pages; i++) {
            page_id_t global_page_id = new_table->entries[i].id;

            /* Set owner in directory using hash table API */
            directory_set_owner(dir, global_page_id, ctx->node_id);

            /* Set owner in page table (ctx->lock already held) */
            new_table->entries[i].owner = ctx->node_id;
        }
        LOG_INFO("Set node %u as owner of %zu pages", ctx->node_id, num_pages);
    }

    pthread_mutex_unlock(&ctx->lock);

    LOG_INFO("dsm_malloc: allocated %zu pages at %p", num_pages, addr);

    /* CRITICAL FIX #1 & BUG FIX (BUG 3): Broadcast allocation and wait for ACKs
     * Must serialize this entire section with allocation_lock to prevent concurrent
     * allocations from corrupting the shared alloc_tracker state */
    if (ctx->config.num_nodes > 1) {
        /* BUG FIX (BUG 3): Acquire allocation lock to serialize use of alloc_tracker */
        pthread_mutex_lock(&ctx->allocation_lock);

        page_id_t start_page_id = new_table->start_page_id;
        page_id_t end_page_id = start_page_id + num_pages - 1;

        /* Count connected nodes (excluding self) */
        int expected_acks = 0;
        pthread_mutex_lock(&ctx->lock);
        for (int i = 0; i < MAX_NODES; i++) {
            if (ctx->network.nodes[i].connected &&
                ctx->network.nodes[i].id != ctx->node_id &&
                !ctx->network.nodes[i].is_failed) {
                expected_acks++;
            }
        }
        pthread_mutex_unlock(&ctx->lock);

        LOG_INFO("Broadcasting SVAS allocation: pages %lu-%lu at addr=%p, size=%zu (owner=node %u, expecting %d ACKs)",
                 start_page_id, end_page_id, addr, aligned_size, ctx->node_id, expected_acks);

        int rc = send_alloc_notify(start_page_id, end_page_id, ctx->node_id, num_pages, addr, aligned_size);
        if (rc != DSM_SUCCESS) {
            LOG_WARN("Failed to broadcast allocation notification");
            pthread_mutex_unlock(&ctx->allocation_lock);  /* BUG FIX: Release lock before return */
            /* Don't fail allocation, but don't wait for ACKs */
            return addr;
        }

        /* Wait for all nodes to ACK the allocation (with 2 second timeout - LAN optimized)
         * This uses the shared alloc_tracker, which is now protected by allocation_lock */
        if (expected_acks > 0) {
            rc = wait_for_alloc_acks(start_page_id, end_page_id, expected_acks, 2);
            if (rc != DSM_SUCCESS) {
                /* Log which nodes didn't ACK */
                pthread_mutex_lock(&ctx->network.alloc_tracker.lock);
                for (int i = 0; i < MAX_NODES; i++) {
                    if (!ctx->network.alloc_tracker.acks_received[i] &&
                        ctx->network.nodes[i].connected &&
                        ctx->network.nodes[i].id != ctx->node_id) {
                        LOG_ERROR("Node %u did not ACK allocation", ctx->network.nodes[i].id);
                    }
                }
                pthread_mutex_unlock(&ctx->network.alloc_tracker.lock);

                LOG_ERROR("Failed to receive all ALLOC_ACKs (timed out after 2s). Aborting allocation.");
                
                pthread_mutex_unlock(&ctx->allocation_lock);
                dsm_free(addr);
                return NULL;
            }
        }

        /* BUG FIX (BUG 3): Release allocation lock after tracker is done */
        pthread_mutex_unlock(&ctx->allocation_lock);
    }

    return addr;
}

int dsm_free(void *ptr) {
    if (!ptr) {
        return DSM_SUCCESS;
    }

    dsm_context_t *ctx = dsm_get_context();
    if (!ctx->initialized || ctx->num_allocations == 0) {
        LOG_ERROR("DSM not initialized or no allocations");
        return DSM_ERROR_INVALID;
    }

    pthread_mutex_lock(&ctx->lock);

    /* Find which page table owns this pointer */
    page_table_t *target_table = NULL;
    int target_index = -1;

    for (int i = 0; i < ctx->num_allocations; i++) {
        if (ctx->page_tables[i] && ctx->page_tables[i]->base_addr == ptr) {
            target_table = ctx->page_tables[i];
            target_index = i;
            break;
        }
    }

    if (!target_table) {
        pthread_mutex_unlock(&ctx->lock);
        LOG_ERROR("dsm_free: invalid pointer %p (not a DSM allocation)", ptr);
        return DSM_ERROR_INVALID;
    }

    size_t size = target_table->total_size;
    page_id_t start_page_id = target_table->start_page_id;
    size_t num_pages = target_table->num_pages;

    /* CRITICAL FIX: Determine if this is a local allocation by checking page ID range
     * Local allocations have page IDs in range [node_id * 1000000, (node_id + 1) * 1000000) */
    page_id_t local_page_start = (page_id_t)ctx->node_id * 1000000;
    page_id_t local_page_end = ((page_id_t)ctx->node_id + 1) * 1000000;
    bool is_local_allocation = (start_page_id >= local_page_start && start_page_id < local_page_end);

    /* Clean up directory entries for all pages in this allocation
     * This prevents memory leak in the directory */
    page_directory_t *dir = get_page_directory();
    if (dir) {
        for (size_t i = 0; i < num_pages; i++) {
            page_id_t page_id = start_page_id + i;
            directory_remove_entry(dir, page_id);
        }
        LOG_DEBUG("Removed %zu directory entries for freed allocation", num_pages);
    }

    /* Remove from list and compact - this prevents new references from being acquired */
    for (int i = target_index; i < ctx->num_allocations - 1; i++) {
        ctx->page_tables[i] = ctx->page_tables[i + 1];
    }
    ctx->page_tables[ctx->num_allocations - 1] = NULL;
    ctx->num_allocations--;

    /* CRITICAL FIX: Only decrement local allocation counter for local allocations */
    if (is_local_allocation && ctx->num_local_allocations > 0) {
        ctx->num_local_allocations--;
    }

    /* Update primary page table reference */
    if (ctx->num_allocations > 0) {
        ctx->page_table = ctx->page_tables[0];
    } else {
        ctx->page_table = NULL;
    }

    pthread_mutex_unlock(&ctx->lock);

    /* Release the owner's reference - table will be destroyed when refcount reaches 0
     * If handlers are currently using the table, it will be destroyed when they release */
    usleep(10000); // Grace period to allow concurrent faults to finish using the table
    page_table_release(target_table);

    /* Unmap memory */
    if (munmap(ptr, size) != 0) {
        LOG_ERROR("munmap failed");
        return DSM_ERROR_MEMORY;
    }

    LOG_INFO("dsm_free: freed %zu bytes at %p (%zu directory entries cleaned)",
             size, ptr, num_pages);
    return DSM_SUCCESS;
}

void* dsm_get_allocation(int index) {
    dsm_context_t *ctx = dsm_get_context();
    void *addr = NULL;

    pthread_mutex_lock(&ctx->lock);
    if (index >= 0 && index < ctx->num_allocations) {
        if (ctx->page_tables[index]) {
            addr = ctx->page_tables[index]->base_addr;
        }
    }
    pthread_mutex_unlock(&ctx->lock);
    
    return addr;
}

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

    page_table_t *new_table = page_table_create(addr, aligned_size, ctx->node_id, ctx->num_allocations);
    if (!new_table) {
        pthread_mutex_unlock(&ctx->lock);
        munmap(addr, aligned_size);
        LOG_ERROR("Failed to create page table");
        return NULL;
    }

    /* Add to list of page tables */
    ctx->page_tables[ctx->num_allocations] = new_table;
    ctx->num_allocations++;

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

    /* Broadcast allocation to all other nodes if multi-node system */
    if (ctx->config.num_nodes > 1) {
        page_id_t start_page_id = new_table->start_page_id;
        page_id_t end_page_id = start_page_id + num_pages - 1;

        LOG_INFO("Broadcasting allocation: pages %lu-%lu (owner=node %u)",
                 start_page_id, end_page_id, ctx->node_id);

        int rc = send_alloc_notify(start_page_id, end_page_id, ctx->node_id, num_pages);
        if (rc != DSM_SUCCESS) {
            LOG_WARN("Failed to broadcast allocation notification");
        }

        /* Give other nodes time to process the notification */
        usleep(50000);  /* 50ms */
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

    /* Remove from list and compact - this prevents new references from being acquired */
    for (int i = target_index; i < ctx->num_allocations - 1; i++) {
        ctx->page_tables[i] = ctx->page_tables[i + 1];
    }
    ctx->page_tables[ctx->num_allocations - 1] = NULL;
    ctx->num_allocations--;

    /* Update primary page table reference */
    if (ctx->num_allocations > 0) {
        ctx->page_table = ctx->page_tables[0];
    } else {
        ctx->page_table = NULL;
    }

    pthread_mutex_unlock(&ctx->lock);

    /* Release the owner's reference - table will be destroyed when refcount reaches 0
     * If handlers are currently using the table, it will be destroyed when they release */
    page_table_release(target_table);

    /* Unmap memory */
    if (munmap(ptr, size) != 0) {
        LOG_ERROR("munmap failed");
        return DSM_ERROR_MEMORY;
    }

    LOG_INFO("dsm_free: freed %zu bytes at %p", size, ptr);
    return DSM_SUCCESS;
}

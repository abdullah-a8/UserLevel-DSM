/**
 * @file allocator.c
 * @brief DSM memory allocator
 */

#include "dsm/dsm.h"
#include "../core/dsm_context.h"
#include "../core/log.h"
#include "page_table.h"
#include <sys/mman.h>
#include <stdlib.h>
#include <string.h>

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

    /* Create or extend page table */
    pthread_mutex_lock(&ctx->lock);

    if (ctx->page_table == NULL) {
        ctx->page_table = page_table_create(addr, aligned_size);
        if (!ctx->page_table) {
            pthread_mutex_unlock(&ctx->lock);
            munmap(addr, aligned_size);
            LOG_ERROR("Failed to create page table");
            return NULL;
        }
    } else {
        /* For now, only support single allocation */
        pthread_mutex_unlock(&ctx->lock);
        munmap(addr, aligned_size);
        LOG_ERROR("Multiple allocations not yet supported");
        return NULL;
    }

    pthread_mutex_unlock(&ctx->lock);

    LOG_INFO("dsm_malloc: allocated %zu pages at %p", num_pages, addr);
    return addr;
}

int dsm_free(void *ptr) {
    if (!ptr) {
        return DSM_SUCCESS;
    }

    dsm_context_t *ctx = dsm_get_context();
    if (!ctx->initialized || !ctx->page_table) {
        LOG_ERROR("DSM not initialized or no allocations");
        return DSM_ERROR_INVALID;
    }

    pthread_mutex_lock(&ctx->lock);

    /* Verify pointer is the base address */
    if (ptr != ctx->page_table->base_addr) {
        pthread_mutex_unlock(&ctx->lock);
        LOG_ERROR("dsm_free: invalid pointer %p (expected %p)",
                  ptr, ctx->page_table->base_addr);
        return DSM_ERROR_INVALID;
    }

    size_t size = ctx->page_table->total_size;

    /* Destroy page table */
    page_table_destroy(ctx->page_table);
    ctx->page_table = NULL;

    pthread_mutex_unlock(&ctx->lock);

    /* Unmap memory */
    if (munmap(ptr, size) != 0) {
        LOG_ERROR("munmap failed");
        return DSM_ERROR_MEMORY;
    }

    LOG_INFO("dsm_free: freed %zu bytes at %p", size, ptr);
    return DSM_SUCCESS;
}

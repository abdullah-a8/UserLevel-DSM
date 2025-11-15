/**
 * @file permission.c
 * @brief Page permission management implementation
 */

#include "permission.h"
#include "page_table.h"
#include "../core/log.h"
#include "../core/dsm_context.h"
#include <sys/mman.h>
#include <errno.h>
#include <string.h>

int get_prot_flags(page_perm_t perm) {
    switch (perm) {
        case PAGE_PERM_NONE:
            return PROT_NONE;
        case PAGE_PERM_READ:
            return PROT_READ;
        case PAGE_PERM_READ_WRITE:
            return PROT_READ | PROT_WRITE;
        default:
            return PROT_NONE;
    }
}

int set_page_permission(void *addr, page_perm_t perm) {
    if (!addr) {
        LOG_ERROR("NULL address");
        return DSM_ERROR_INVALID;
    }

    dsm_context_t *ctx = dsm_get_context();
    if (!ctx->initialized || ctx->num_allocations == 0) {
        LOG_ERROR("DSM not initialized");
        return DSM_ERROR_INIT;
    }

    /* Align to page boundary */
    void *page_base = page_get_base_addr(addr);

    /* Get protection flags */
    int prot = get_prot_flags(perm);

    /* Change permissions */
    if (mprotect(page_base, PAGE_SIZE, prot) != 0) {
        LOG_ERROR("mprotect failed: %s", strerror(errno));
        return DSM_ERROR_PERMISSION;
    }

    /* Search all page tables for this address */
    page_entry_t *entry = NULL;
    page_table_t *owning_table = NULL;
    pthread_mutex_lock(&ctx->lock);
    for (int i = 0; i < ctx->num_allocations; i++) {
        if (ctx->page_tables[i]) {
            entry = page_table_lookup_by_addr(ctx->page_tables[i], page_base);
            if (entry) {
                owning_table = ctx->page_tables[i];
                break;
            }
        }
    }
    pthread_mutex_unlock(&ctx->lock);

    /* Update page table state */
    if (entry && owning_table) {
        pthread_mutex_lock(&owning_table->lock);

        switch (perm) {
            case PAGE_PERM_NONE:
                entry->state = PAGE_STATE_INVALID;
                break;
            case PAGE_PERM_READ:
                entry->state = PAGE_STATE_READ_ONLY;
                break;
            case PAGE_PERM_READ_WRITE:
                entry->state = PAGE_STATE_READ_WRITE;
                break;
        }

        pthread_mutex_unlock(&owning_table->lock);

        LOG_DEBUG("Page %p permission set to %d (state=%d)",
                  page_base, perm, entry->state);
    }

    return DSM_SUCCESS;
}

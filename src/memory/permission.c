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
    if (!ctx->initialized || !ctx->page_table) {
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

    /* Update page table state */
    page_entry_t *entry = page_table_lookup_by_addr(ctx->page_table, page_base);
    if (entry) {
        pthread_mutex_lock(&ctx->page_table->lock);

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

        pthread_mutex_unlock(&ctx->page_table->lock);

        LOG_DEBUG("Page %p permission set to %d (state=%d)",
                  page_base, perm, entry->state);
    }

    return DSM_SUCCESS;
}

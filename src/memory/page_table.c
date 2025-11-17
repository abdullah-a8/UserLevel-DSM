/**
 * @file page_table.c
 * @brief Page table implementation
 */

#include "page_table.h"
#include "../core/log.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

page_table_t* page_table_create(void *base_addr, size_t size, node_id_t node_id, int allocation_index) {
    if (!base_addr || size == 0) {
        LOG_ERROR("Invalid parameters: base_addr=%p, size=%zu", base_addr, size);
        return NULL;
    }

    if (allocation_index < 0 || allocation_index >= 32) {
        LOG_ERROR("Invalid allocation_index: %d (must be 0-31)", allocation_index);
        return NULL;
    }

    page_table_t *table = malloc(sizeof(page_table_t));
    if (!table) {
        LOG_ERROR("Failed to allocate page table");
        return NULL;
    }

    table->base_addr = base_addr;
    table->total_size = size;
    table->num_pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;

    /* Each node gets 1M page IDs, divided into 32 allocation slots of ~31,250 pages each
     * This allows up to 32 allocations per node with unique page IDs
     * Each allocation can have up to ~31,250 pages (128MB with 4KB pages) */
    table->start_page_id = (page_id_t)node_id * 1000000 + allocation_index * 31250;

    /* Verify allocation doesn't exceed the page ID space for this allocation slot */
    if (table->num_pages > 31250) {
        LOG_ERROR("Allocation too large: %zu pages exceeds limit of 31,250 pages per allocation",
                  table->num_pages);
        free(table);
        return NULL;
    }

    table->entries = calloc(table->num_pages, sizeof(page_entry_t));
    if (!table->entries) {
        LOG_ERROR("Failed to allocate page entries");
        free(table);
        return NULL;
    }

    pthread_mutex_init(&table->lock, NULL);
    table->refcount = 1;  /* Initial reference held by creator */

    /* Initialize all entries with globally unique page IDs */
    for (size_t i = 0; i < table->num_pages; i++) {
        table->entries[i].id = table->start_page_id + i;
        table->entries[i].local_addr = (char*)base_addr + (i * PAGE_SIZE);
        table->entries[i].owner = node_id;
        table->entries[i].state = PAGE_STATE_INVALID;
        table->entries[i].version = 0;
        table->entries[i].is_allocated = true;
        table->entries[i].request_pending = false;
        table->entries[i].num_waiting_threads = 0;
        table->entries[i].fetch_result = DSM_SUCCESS;  /* Initialize to success */
        table->entries[i].pending_inv_acks = 0;
        pthread_cond_init(&table->entries[i].ready_cv, NULL);
        pthread_cond_init(&table->entries[i].inv_ack_cv, NULL);
        pthread_mutex_init(&table->entries[i].entry_lock, NULL);
    }

    LOG_INFO("Page table created: base=%p, size=%zu, pages=%zu, id_range=%lu-%lu",
             base_addr, size, table->num_pages, table->start_page_id,
             table->start_page_id + table->num_pages - 1);
    return table;
}

page_table_t* page_table_create_remote(void *base_addr, size_t size, node_id_t owner, page_id_t start_page_id) {
    if (!base_addr || size == 0) {
        LOG_ERROR("Invalid parameters: base_addr=%p, size=%zu", base_addr, size);
        return NULL;
    }

    page_table_t *table = malloc(sizeof(page_table_t));
    if (!table) {
        LOG_ERROR("Failed to allocate page table");
        return NULL;
    }

    table->base_addr = base_addr;
    table->total_size = size;
    table->num_pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    table->start_page_id = start_page_id;  /* Use remote node's page IDs */

    table->entries = calloc(table->num_pages, sizeof(page_entry_t));
    if (!table->entries) {
        LOG_ERROR("Failed to allocate page entries");
        free(table);
        return NULL;
    }

    pthread_mutex_init(&table->lock, NULL);
    table->refcount = 1;

    /* Initialize all entries with remote page IDs and owner */
    for (size_t i = 0; i < table->num_pages; i++) {
        table->entries[i].id = start_page_id + i;
        table->entries[i].local_addr = (char*)base_addr + (i * PAGE_SIZE);
        table->entries[i].owner = owner;  /* Remote owner */
        table->entries[i].state = PAGE_STATE_INVALID;  /* Start invalid, will fetch on fault */
        table->entries[i].version = 0;
        table->entries[i].is_allocated = true;
        table->entries[i].request_pending = false;
        table->entries[i].num_waiting_threads = 0;
        table->entries[i].fetch_result = DSM_SUCCESS;  /* Initialize to success */
        table->entries[i].pending_inv_acks = 0;
        pthread_cond_init(&table->entries[i].ready_cv, NULL);
        pthread_cond_init(&table->entries[i].inv_ack_cv, NULL);
        pthread_mutex_init(&table->entries[i].entry_lock, NULL);
    }

    LOG_INFO("Remote page table created: base=%p, size=%zu, pages=%zu, id_range=%lu-%lu (owner=node %u)",
             base_addr, size, table->num_pages, start_page_id,
             start_page_id + table->num_pages - 1, owner);
    return table;
}

void page_table_destroy(page_table_t *table) {
    if (!table) {
        return;
    }

    if (table->entries) {
        for (size_t i = 0; i < table->num_pages; i++) {
            pthread_cond_destroy(&table->entries[i].ready_cv);
            pthread_cond_destroy(&table->entries[i].inv_ack_cv);
            pthread_mutex_destroy(&table->entries[i].entry_lock);
        }
        free(table->entries);
    }

    pthread_mutex_destroy(&table->lock);
    free(table);
    LOG_DEBUG("Page table destroyed");
}

void page_table_acquire(page_table_t *table) {
    if (!table) {
        return;
    }

    pthread_mutex_lock(&table->lock);
    table->refcount++;
    LOG_DEBUG("Page table refcount incremented to %d", table->refcount);
    pthread_mutex_unlock(&table->lock);
}

void page_table_release(page_table_t *table) {
    if (!table) {
        return;
    }

    pthread_mutex_lock(&table->lock);
    table->refcount--;
    int count = table->refcount;
    LOG_DEBUG("Page table refcount decremented to %d", count);
    pthread_mutex_unlock(&table->lock);

    /* If refcount reached 0, destroy the table */
    if (count == 0) {
        LOG_INFO("Page table refcount reached 0, destroying");
        page_table_destroy(table);
    }
}

page_entry_t* page_table_lookup_by_addr(page_table_t *table, void *addr) {
    if (!table || !addr) {
        return NULL;
    }

    /* Check if address is within range */
    if (addr < table->base_addr ||
        addr >= (char*)table->base_addr + table->total_size) {
        return NULL;
    }

    /* Calculate page index */
    size_t offset = (char*)addr - (char*)table->base_addr;
    size_t page_idx = offset / PAGE_SIZE;

    if (page_idx >= table->num_pages) {
        return NULL;
    }

    return &table->entries[page_idx];
}

page_entry_t* page_table_lookup_by_id(page_table_t *table, page_id_t page_id) {
    if (!table) {
        return NULL;
    }

    /* Check if page_id is in this table's range */
    if (page_id < table->start_page_id ||
        page_id >= table->start_page_id + table->num_pages) {
        return NULL;
    }

    /* Convert global page ID to local array index */
    size_t local_idx = page_id - table->start_page_id;
    return &table->entries[local_idx];
}

int page_table_set_owner(page_table_t *table, page_id_t page_id, node_id_t owner) {
    pthread_mutex_lock(&table->lock);

    page_entry_t *entry = page_table_lookup_by_id(table, page_id);
    if (!entry) {
        pthread_mutex_unlock(&table->lock);
        return DSM_ERROR_NOT_FOUND;
    }

    entry->owner = owner;
    LOG_DEBUG("Page %lu owner set to node %u", page_id, owner);

    pthread_mutex_unlock(&table->lock);
    return DSM_SUCCESS;
}

int page_table_set_state(page_table_t *table, page_id_t page_id, page_state_t state) {
    pthread_mutex_lock(&table->lock);

    page_entry_t *entry = page_table_lookup_by_id(table, page_id);
    if (!entry) {
        pthread_mutex_unlock(&table->lock);
        return DSM_ERROR_NOT_FOUND;
    }

    entry->state = state;
    LOG_DEBUG("Page %lu state set to %d", page_id, state);

    pthread_mutex_unlock(&table->lock);
    return DSM_SUCCESS;
}

void* page_get_base_addr(void *addr) {
    uintptr_t page_mask = ~(PAGE_SIZE - 1);
    return (void*)((uintptr_t)addr & page_mask);
}

page_id_t page_addr_to_id(page_table_t *table, void *addr) {
    if (!table || !addr) {
        return (page_id_t)-1;
    }

    size_t offset = (char*)addr - (char*)table->base_addr;
    return table->start_page_id + (offset / PAGE_SIZE);
}

void* page_id_to_addr(page_table_t *table, page_id_t page_id) {
    if (!table) {
        return NULL;
    }

    /* Check if page_id is in this table's range */
    if (page_id < table->start_page_id ||
        page_id >= table->start_page_id + table->num_pages) {
        return NULL;
    }

    /* Convert global page ID to local index */
    size_t local_idx = page_id - table->start_page_id;
    return (char*)table->base_addr + (local_idx * PAGE_SIZE);
}

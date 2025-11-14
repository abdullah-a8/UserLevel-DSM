/**
 * @file page_table.c
 * @brief Page table implementation
 */

#include "page_table.h"
#include "../core/log.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

page_table_t* page_table_create(void *base_addr, size_t size) {
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

    table->entries = calloc(table->num_pages, sizeof(page_entry_t));
    if (!table->entries) {
        LOG_ERROR("Failed to allocate page entries");
        free(table);
        return NULL;
    }

    pthread_mutex_init(&table->lock, NULL);

    /* Initialize all entries */
    for (size_t i = 0; i < table->num_pages; i++) {
        table->entries[i].id = i;
        table->entries[i].local_addr = (char*)base_addr + (i * PAGE_SIZE);
        table->entries[i].owner = 0;
        table->entries[i].state = PAGE_STATE_INVALID;
        table->entries[i].version = 0;
        table->entries[i].is_allocated = true;
        table->entries[i].request_pending = false;
        pthread_cond_init(&table->entries[i].ready_cv, NULL);
    }

    LOG_INFO("Page table created: base=%p, size=%zu, pages=%zu",
             base_addr, size, table->num_pages);
    return table;
}

void page_table_destroy(page_table_t *table) {
    if (!table) {
        return;
    }

    if (table->entries) {
        for (size_t i = 0; i < table->num_pages; i++) {
            pthread_cond_destroy(&table->entries[i].ready_cv);
        }
        free(table->entries);
    }

    pthread_mutex_destroy(&table->lock);
    free(table);
    LOG_DEBUG("Page table destroyed");
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
    if (!table || page_id >= table->num_pages) {
        return NULL;
    }

    return &table->entries[page_id];
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
    return offset / PAGE_SIZE;
}

void* page_id_to_addr(page_table_t *table, page_id_t page_id) {
    if (!table || page_id >= table->num_pages) {
        return NULL;
    }

    return (char*)table->base_addr + (page_id * PAGE_SIZE);
}

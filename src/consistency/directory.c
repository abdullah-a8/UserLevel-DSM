/**
 * @file directory.c
 * @brief Page directory implementation
 */

#include "directory.h"
#include "../core/log.h"
#include <stdlib.h>
#include <string.h>

page_directory_t* directory_create(size_t num_pages) {
    page_directory_t *dir = malloc(sizeof(page_directory_t));
    if (!dir) {
        LOG_ERROR("Failed to allocate directory");
        return NULL;
    }

    dir->entries = calloc(num_pages, sizeof(directory_entry_t));
    if (!dir->entries) {
        LOG_ERROR("Failed to allocate directory entries");
        free(dir);
        return NULL;
    }

    dir->num_entries = num_pages;
    pthread_mutex_init(&dir->lock, NULL);

    /* Initialize each entry - owner will be set when pages are allocated */
    for (size_t i = 0; i < num_pages; i++) {
        dir->entries[i].page_id = i;
        dir->entries[i].owner = (node_id_t)-1;  /* Invalid owner initially */
        dir->entries[i].sharers.count = 0;
        dir->entries[i].is_valid = true;
        pthread_mutex_init(&dir->entries[i].lock, NULL);
    }

    LOG_INFO("Created page directory with %zu entries", num_pages);
    return dir;
}

void directory_destroy(page_directory_t *dir) {
    if (!dir) return;

    for (size_t i = 0; i < dir->num_entries; i++) {
        pthread_mutex_destroy(&dir->entries[i].lock);
    }

    pthread_mutex_destroy(&dir->lock);
    free(dir->entries);
    free(dir);
}

int directory_lookup(page_directory_t *dir, page_id_t page_id, node_id_t *owner) {
    if (!dir || !owner) {
        return DSM_ERROR_INVALID;
    }

    if (page_id >= dir->num_entries) {
        LOG_ERROR("Page ID %lu out of range", page_id);
        return DSM_ERROR_NOT_FOUND;
    }

    directory_entry_t *entry = &dir->entries[page_id];

    pthread_mutex_lock(&entry->lock);
    *owner = entry->owner;
    pthread_mutex_unlock(&entry->lock);

    return DSM_SUCCESS;
}

int directory_add_reader(page_directory_t *dir, page_id_t page_id, node_id_t reader) {
    if (!dir) {
        return DSM_ERROR_INVALID;
    }

    if (page_id >= dir->num_entries) {
        return DSM_ERROR_NOT_FOUND;
    }

    directory_entry_t *entry = &dir->entries[page_id];

    pthread_mutex_lock(&entry->lock);

    /* Check if already in sharer list */
    for (int i = 0; i < entry->sharers.count; i++) {
        if (entry->sharers.nodes[i] == reader) {
            pthread_mutex_unlock(&entry->lock);
            return DSM_SUCCESS;  /* Already a sharer */
        }
    }

    /* Add to sharer list */
    if (entry->sharers.count < MAX_SHARERS) {
        entry->sharers.nodes[entry->sharers.count++] = reader;
        LOG_DEBUG("Added node %u as sharer for page %lu", reader, page_id);
    } else {
        LOG_WARN("Sharer list full for page %lu", page_id);
        pthread_mutex_unlock(&entry->lock);
        return DSM_ERROR_BUSY;
    }

    pthread_mutex_unlock(&entry->lock);
    return DSM_SUCCESS;
}

int directory_set_writer(page_directory_t *dir, page_id_t page_id, node_id_t writer,
                         node_id_t *invalidate_list, int *num_invalidate) {
    if (!dir || !invalidate_list || !num_invalidate) {
        return DSM_ERROR_INVALID;
    }

    if (page_id >= dir->num_entries) {
        return DSM_ERROR_NOT_FOUND;
    }

    directory_entry_t *entry = &dir->entries[page_id];

    pthread_mutex_lock(&entry->lock);

    /* Build invalidation list from current sharers */
    *num_invalidate = 0;
    for (int i = 0; i < entry->sharers.count; i++) {
        node_id_t sharer = entry->sharers.nodes[i];
        if (sharer != writer) {  /* Don't invalidate the new writer */
            invalidate_list[(*num_invalidate)++] = sharer;
        }
    }

    /* If current owner is different and not the writer, add to invalidation list */
    if (entry->owner != writer && entry->owner != 0) {
        bool already_in_list = false;
        for (int i = 0; i < *num_invalidate; i++) {
            if (invalidate_list[i] == entry->owner) {
                already_in_list = true;
                break;
            }
        }
        if (!already_in_list) {
            invalidate_list[(*num_invalidate)++] = entry->owner;
        }
    }

    /* Set new owner and clear sharers */
    entry->owner = writer;
    entry->sharers.count = 0;

    LOG_DEBUG("Set node %u as writer for page %lu (%d nodes to invalidate)",
              writer, page_id, *num_invalidate);

    pthread_mutex_unlock(&entry->lock);
    return DSM_SUCCESS;
}

int directory_remove_sharer(page_directory_t *dir, page_id_t page_id, node_id_t node) {
    if (!dir) {
        return DSM_ERROR_INVALID;
    }

    if (page_id >= dir->num_entries) {
        return DSM_ERROR_NOT_FOUND;
    }

    directory_entry_t *entry = &dir->entries[page_id];

    pthread_mutex_lock(&entry->lock);

    /* Find and remove node from sharer list */
    for (int i = 0; i < entry->sharers.count; i++) {
        if (entry->sharers.nodes[i] == node) {
            /* Shift remaining elements */
            for (int j = i; j < entry->sharers.count - 1; j++) {
                entry->sharers.nodes[j] = entry->sharers.nodes[j + 1];
            }
            entry->sharers.count--;
            LOG_DEBUG("Removed node %u from sharers of page %lu", node, page_id);
            break;
        }
    }

    pthread_mutex_unlock(&entry->lock);
    return DSM_SUCCESS;
}

int directory_get_sharers(page_directory_t *dir, page_id_t page_id,
                          node_id_t *sharers, int *count) {
    if (!dir || !sharers || !count) {
        return DSM_ERROR_INVALID;
    }

    if (page_id >= dir->num_entries) {
        return DSM_ERROR_NOT_FOUND;
    }

    directory_entry_t *entry = &dir->entries[page_id];

    pthread_mutex_lock(&entry->lock);

    *count = entry->sharers.count;
    for (int i = 0; i < entry->sharers.count; i++) {
        sharers[i] = entry->sharers.nodes[i];
    }

    pthread_mutex_unlock(&entry->lock);
    return DSM_SUCCESS;
}

/**
 * @file directory.c
 * @brief Page directory implementation (hash table-based)
 */

#include "directory.h"
#include "../core/log.h"
#include <stdlib.h>
#include <string.h>

/* Hash function for page IDs */
static inline size_t hash_page_id(page_id_t page_id, size_t table_size) {
    return page_id % table_size;
}

/* Find entry in hash table, or return NULL if not found */
static directory_entry_t* find_entry(page_directory_t *dir, page_id_t page_id) {
    size_t bucket = hash_page_id(page_id, dir->table_size);
    directory_entry_t *entry = dir->buckets[bucket];

    while (entry != NULL) {
        if (entry->page_id == page_id) {
            return entry;
        }
        entry = entry->next;
    }

    return NULL;
}

/* Find or create entry in hash table */
static directory_entry_t* find_or_create_entry(page_directory_t *dir, page_id_t page_id) {
    size_t bucket = hash_page_id(page_id, dir->table_size);
    directory_entry_t *entry = dir->buckets[bucket];

    /* Search for existing entry */
    while (entry != NULL) {
        if (entry->page_id == page_id) {
            return entry;
        }
        entry = entry->next;
    }

    /* Entry not found, create new one */
    entry = malloc(sizeof(directory_entry_t));
    if (!entry) {
        LOG_ERROR("Failed to allocate directory entry for page %lu", page_id);
        return NULL;
    }

    /* Initialize entry */
    entry->page_id = page_id;
    entry->owner = (node_id_t)-1;  /* Invalid owner initially */
    entry->sharers.count = 0;
    entry->is_valid = true;
    pthread_mutex_init(&entry->lock, NULL);

    /* Insert at head of bucket chain */
    entry->next = dir->buckets[bucket];
    dir->buckets[bucket] = entry;
    dir->num_entries++;

    LOG_DEBUG("Created directory entry for page %lu (bucket %zu, total entries: %zu)",
              page_id, bucket, dir->num_entries);

    return entry;
}

page_directory_t* directory_create(size_t table_size) {
    page_directory_t *dir = malloc(sizeof(page_directory_t));
    if (!dir) {
        LOG_ERROR("Failed to allocate directory");
        return NULL;
    }

    /* Allocate hash table buckets (initialized to NULL) */
    dir->buckets = calloc(table_size, sizeof(directory_entry_t*));
    if (!dir->buckets) {
        LOG_ERROR("Failed to allocate directory buckets");
        free(dir);
        return NULL;
    }

    dir->table_size = table_size;
    dir->num_entries = 0;  /* Entries are created on-demand */
    pthread_mutex_init(&dir->lock, NULL);

    LOG_INFO("Created page directory with %zu buckets (hash table)", table_size);
    return dir;
}

void directory_destroy(page_directory_t *dir) {
    if (!dir) return;

    /* Free all entries in hash table */
    for (size_t i = 0; i < dir->table_size; i++) {
        directory_entry_t *entry = dir->buckets[i];
        while (entry != NULL) {
            directory_entry_t *next = entry->next;
            pthread_mutex_destroy(&entry->lock);
            free(entry);
            entry = next;
        }
    }

    pthread_mutex_destroy(&dir->lock);
    free(dir->buckets);
    free(dir);
}

int directory_lookup(page_directory_t *dir, page_id_t page_id, node_id_t *owner) {
    if (!dir || !owner) {
        return DSM_ERROR_INVALID;
    }

    /* Find entry in hash table, create if doesn't exist */
    directory_entry_t *entry = find_or_create_entry(dir, page_id);
    if (!entry) {
        return DSM_ERROR_MEMORY;
    }

    pthread_mutex_lock(&entry->lock);
    *owner = entry->owner;
    pthread_mutex_unlock(&entry->lock);

    return DSM_SUCCESS;
}

int directory_add_reader(page_directory_t *dir, page_id_t page_id, node_id_t reader) {
    if (!dir) {
        return DSM_ERROR_INVALID;
    }

    /* Find or create entry in hash table */
    directory_entry_t *entry = find_or_create_entry(dir, page_id);
    if (!entry) {
        return DSM_ERROR_MEMORY;
    }

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

    /* Find or create entry in hash table */
    directory_entry_t *entry = find_or_create_entry(dir, page_id);
    if (!entry) {
        return DSM_ERROR_MEMORY;
    }

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

    /* Only search for entry, don't create if doesn't exist */
    directory_entry_t *entry = find_entry(dir, page_id);
    if (!entry) {
        return DSM_SUCCESS;  /* Entry doesn't exist, nothing to remove */
    }

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

    /* Only search for entry, don't create if doesn't exist */
    directory_entry_t *entry = find_entry(dir, page_id);
    if (!entry) {
        *count = 0;  /* No entry means no sharers */
        return DSM_SUCCESS;
    }

    pthread_mutex_lock(&entry->lock);

    *count = entry->sharers.count;
    for (int i = 0; i < entry->sharers.count; i++) {
        sharers[i] = entry->sharers.nodes[i];
    }

    pthread_mutex_unlock(&entry->lock);
    return DSM_SUCCESS;
}

int directory_set_owner(page_directory_t *dir, page_id_t page_id, node_id_t owner) {
    if (!dir) {
        return DSM_ERROR_INVALID;
    }

    /* Find or create entry in hash table */
    directory_entry_t *entry = find_or_create_entry(dir, page_id);
    if (!entry) {
        return DSM_ERROR_MEMORY;
    }

    pthread_mutex_lock(&entry->lock);
    entry->owner = owner;
    pthread_mutex_unlock(&entry->lock);

    return DSM_SUCCESS;
}

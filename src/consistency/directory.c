/**
 * @file directory.c
 * @brief Page directory implementation (hash table-based)
 */

#include "directory.h"
#include "page_migration.h"
#include "../core/log.h"
#include "../core/dsm_context.h"
#include "../network/handlers.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>

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
    if (entry->owner != writer && entry->owner != (node_id_t)-1) {
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

    /* Set new owner but DO NOT clear sharers yet
     * Sharers will be cleared after all invalidation ACKs are received
     * This prevents a race where new readers could be added before invalidations complete */
    entry->owner = writer;

    LOG_DEBUG("Set node %u as writer for page %lu (%d nodes to invalidate)",
              writer, page_id, *num_invalidate);

    pthread_mutex_unlock(&entry->lock);
    return DSM_SUCCESS;
}

int directory_clear_sharers(page_directory_t *dir, page_id_t page_id) {
    if (!dir) {
        return DSM_ERROR_INVALID;
    }

    /* Find entry (don't create if doesn't exist) */
    directory_entry_t *entry = find_entry(dir, page_id);
    if (!entry) {
        return DSM_SUCCESS;  /* No entry, nothing to clear */
    }

    pthread_mutex_lock(&entry->lock);
    entry->sharers.count = 0;
    LOG_DEBUG("Cleared sharer list for page %lu", page_id);
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

int directory_remove_entry(page_directory_t *dir, page_id_t page_id) {
    if (!dir) {
        return DSM_ERROR_INVALID;
    }

    size_t bucket = hash_page_id(page_id, dir->table_size);

    pthread_mutex_lock(&dir->lock);

    directory_entry_t **curr = &dir->buckets[bucket];

    /* Search for entry in chain */
    while (*curr != NULL) {
        if ((*curr)->page_id == page_id) {
            /* Found it - remove from chain */
            directory_entry_t *to_free = *curr;
            *curr = to_free->next;  /* Unlink from chain */
            dir->num_entries--;

            pthread_mutex_unlock(&dir->lock);

            /* Free the entry (destroy its mutex first) */
            pthread_mutex_destroy(&to_free->lock);
            free(to_free);

            LOG_DEBUG("Removed directory entry for page %lu (total entries: %zu)",
                      page_id, dir->num_entries);
            return DSM_SUCCESS;
        }
        curr = &(*curr)->next;
    }

    pthread_mutex_unlock(&dir->lock);
    return DSM_SUCCESS;  /* Entry not found, nothing to remove */
}

int directory_handle_node_failure(page_directory_t *dir, node_id_t failed_node) {
    if (!dir) {
        return DSM_ERROR_INVALID;
    }

    LOG_INFO("Handling failure of node %u - cleaning up directory", failed_node);

    int pages_cleared = 0;
    int sharers_removed = 0;

    pthread_mutex_lock(&dir->lock);

    /* Iterate through all buckets in hash table */
    for (size_t bucket = 0; bucket < dir->table_size; bucket++) {
        directory_entry_t *entry = dir->buckets[bucket];

        /* Iterate through linked list in this bucket */
        while (entry != NULL) {
            pthread_mutex_lock(&entry->lock);

            /* If failed node owns this page, mark as unowned */
            if (entry->owner == failed_node) {
                entry->owner = (node_id_t)-1;  /* No owner */
                pages_cleared++;
                LOG_DEBUG("Cleared ownership of page %lu (was owned by failed node %u)",
                         entry->page_id, failed_node);
            }

            /* Remove failed node from sharer list */
            for (int i = 0; i < entry->sharers.count; i++) {
                if (entry->sharers.nodes[i] == failed_node) {
                    /* Shift remaining elements */
                    for (int j = i; j < entry->sharers.count - 1; j++) {
                        entry->sharers.nodes[j] = entry->sharers.nodes[j + 1];
                    }
                    entry->sharers.count--;
                    sharers_removed++;
                    LOG_DEBUG("Removed failed node %u from sharers of page %lu",
                             failed_node, entry->page_id);
                    break;  /* Node can only appear once in sharer list */
                }
            }

            pthread_mutex_unlock(&entry->lock);
            entry = entry->next;
        }
    }

    pthread_mutex_unlock(&dir->lock);

    LOG_INFO("Node %u failure cleanup complete: %d pages cleared, %d sharer entries removed",
             failed_node, pages_cleared, sharers_removed);

    return DSM_SUCCESS;
}

int directory_reclaim_ownership(page_directory_t *dir, page_id_t page_id, node_id_t new_owner) {
    if (!dir) {
        return DSM_ERROR_INVALID;
    }

    /* Find or create entry */
    directory_entry_t *entry = find_or_create_entry(dir, page_id);
    if (!entry) {
        return DSM_ERROR_MEMORY;
    }

    pthread_mutex_lock(&entry->lock);

    node_id_t old_owner = entry->owner;

    /* Transfer ownership to new owner */
    entry->owner = new_owner;

    /* Clear sharer list (new owner has exclusive access) */
    entry->sharers.count = 0;

    pthread_mutex_unlock(&entry->lock);

    LOG_INFO("Reclaimed ownership of page %lu: old_owner=%u (failed), new_owner=%u",
             page_id, old_owner, new_owner);

    return DSM_SUCCESS;
}

int query_directory_manager(page_id_t page_id, node_id_t *owner) {
    dsm_context_t *ctx = dsm_get_context();

    /* If we are the manager (Node 0), query local directory */
    if (ctx->node_id == 0) {
        page_directory_t *dir = get_page_directory();
        return directory_lookup(dir, page_id, owner);
    }

    /* If not manager, query manager via network */
    pthread_mutex_lock(&ctx->network.dir_tracker.lock);

    LOG_INFO("Querying directory for page %lu (active=%d, complete=%d)",
             page_id, ctx->network.dir_tracker.active, ctx->network.dir_tracker.complete);

    /* Setup tracker */
    ctx->network.dir_tracker.page_id = page_id;
    ctx->network.dir_tracker.active = true;
    ctx->network.dir_tracker.complete = false;

    /* Send query */
    int rc = send_dir_query(0, page_id);
    if (rc != DSM_SUCCESS) {
        LOG_ERROR("Failed to send DIR_QUERY for page %lu", page_id);
        ctx->network.dir_tracker.active = false;
        pthread_mutex_unlock(&ctx->network.dir_tracker.lock);
        return rc;
    }

    LOG_INFO("Waiting for DIR_REPLY for page %lu...", page_id);

    /* Wait for reply */
    struct timespec timeout;
    clock_gettime(CLOCK_REALTIME, &timeout);
    timeout.tv_sec += 1; /* 1 second timeout */

    while (!ctx->network.dir_tracker.complete) {
        rc = pthread_cond_timedwait(&ctx->network.dir_tracker.cv,
                                   &ctx->network.dir_tracker.lock, &timeout);
        if (rc == ETIMEDOUT) {
            LOG_ERROR("Timeout querying directory manager for page %lu (active=%d, complete=%d)",
                      page_id, ctx->network.dir_tracker.active, ctx->network.dir_tracker.complete);
            ctx->network.dir_tracker.active = false;
            pthread_mutex_unlock(&ctx->network.dir_tracker.lock);
            return DSM_ERROR_TIMEOUT;
        }
    }

    *owner = ctx->network.dir_tracker.owner;
    LOG_INFO("DIR_QUERY complete for page %lu: owner=node %u", page_id, *owner);
    ctx->network.dir_tracker.active = false;

    pthread_mutex_unlock(&ctx->network.dir_tracker.lock);
    return DSM_SUCCESS;
}

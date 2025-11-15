/**
 * @file directory.h
 * @brief Page directory for tracking ownership and sharers
 *
 * This module maintains a centralized directory of page ownership and sharer lists,
 * implementing the single-writer/multiple-reader consistency protocol.
 */

#ifndef DIRECTORY_H
#define DIRECTORY_H

#include "dsm/types.h"
#include <pthread.h>
#include <stdbool.h>

/* Maximum number of sharers per page */
#define MAX_SHARERS 16

/**
 * List of nodes that have read-only copies
 */
typedef struct {
    node_id_t nodes[MAX_SHARERS];
    int count;
} sharer_list_t;

/**
 * Directory entry for one page
 */
typedef struct {
    page_id_t page_id;
    node_id_t owner;           /**< Current owner (has write access) */
    sharer_list_t sharers;     /**< Nodes with read-only copies */
    bool is_valid;             /**< True if entry is in use */
    pthread_mutex_t lock;      /**< Per-entry lock */
} directory_entry_t;

/**
 * Page directory structure
 */
typedef struct page_directory_s {
    directory_entry_t *entries;
    size_t num_entries;
    pthread_mutex_t lock;      /**< Global directory lock */
} page_directory_t;

/**
 * Create a page directory
 *
 * @param num_pages Number of pages to track
 * @return Pointer to directory, or NULL on failure
 */
page_directory_t* directory_create(size_t num_pages);

/**
 * Destroy a page directory
 *
 * @param dir Directory to destroy
 */
void directory_destroy(page_directory_t *dir);

/**
 * Look up the current owner of a page
 *
 * @param dir Page directory
 * @param page_id Page identifier
 * @param owner Output: current owner node ID
 * @return DSM_SUCCESS on success, error code on failure
 */
int directory_lookup(page_directory_t *dir, page_id_t page_id, node_id_t *owner);

/**
 * Update page ownership for read access
 *
 * @param dir Page directory
 * @param page_id Page identifier
 * @param reader Node requesting read access
 * @return DSM_SUCCESS on success, error code on failure
 */
int directory_add_reader(page_directory_t *dir, page_id_t page_id, node_id_t reader);

/**
 * Update page ownership for write access
 *
 * @param dir Page directory
 * @param page_id Page identifier
 * @param writer Node requesting write access
 * @param invalidate_list Output: list of nodes to invalidate
 * @param num_invalidate Output: number of nodes to invalidate
 * @return DSM_SUCCESS on success, error code on failure
 */
int directory_set_writer(page_directory_t *dir, page_id_t page_id, node_id_t writer,
                         node_id_t *invalidate_list, int *num_invalidate);

/**
 * Remove a node from sharers list
 *
 * @param dir Page directory
 * @param page_id Page identifier
 * @param node Node to remove
 * @return DSM_SUCCESS on success, error code on failure
 */
int directory_remove_sharer(page_directory_t *dir, page_id_t page_id, node_id_t node);

/**
 * Get sharer list for a page
 *
 * @param dir Page directory
 * @param page_id Page identifier
 * @param sharers Output: array of sharer node IDs
 * @param count Output: number of sharers
 * @return DSM_SUCCESS on success, error code on failure
 */
int directory_get_sharers(page_directory_t *dir, page_id_t page_id,
                          node_id_t *sharers, int *count);

#endif /* DIRECTORY_H */

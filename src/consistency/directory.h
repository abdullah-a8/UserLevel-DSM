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
 * Directory entry for one page (hash table node with chaining)
 */
typedef struct directory_entry_s {
    page_id_t page_id;
    node_id_t owner;           /**< Current owner (has write access) */
    sharer_list_t sharers;     /**< Nodes with read-only copies */
    bool is_valid;             /**< True if entry is in use */
    pthread_mutex_t lock;      /**< Per-entry lock */
    struct directory_entry_s *next;  /**< Next entry in hash chain */
} directory_entry_t;

/**
 * Page directory structure (hash table implementation)
 */
typedef struct page_directory_s {
    directory_entry_t **buckets;    /**< Hash table buckets (array of linked lists) */
    size_t table_size;              /**< Number of hash buckets */
    size_t num_entries;             /**< Number of pages currently tracked */
    pthread_mutex_t lock;           /**< Global directory lock */
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

/**
 * Set the owner of a page (used during allocation)
 *
 * @param dir Page directory
 * @param page_id Page identifier
 * @param owner New owner node ID
 * @return DSM_SUCCESS on success, error code on failure
 */
int directory_set_owner(page_directory_t *dir, page_id_t page_id, node_id_t owner);

/**
 * Clear all sharers for a page
 * Should be called after all invalidation ACKs are received
 *
 * @param dir Page directory
 * @param page_id Page identifier
 * @return DSM_SUCCESS on success, error code on failure
 */
int directory_clear_sharers(page_directory_t *dir, page_id_t page_id);

/**
 * Remove a directory entry (frees memory)
 * Should be called when a page is freed
 *
 * @param dir Page directory
 * @param page_id Page identifier
 * @return DSM_SUCCESS on success, error code on failure
 */
int directory_remove_entry(page_directory_t *dir, page_id_t page_id);

/**
 * Handle node failure - remove failed node from all directory entries
 * This is called when a node is detected as failed via heartbeat timeout
 * Pages owned by the failed node are marked as having no owner (-1)
 * Failed node is removed from all sharer lists
 *
 * @param dir Page directory
 * @param failed_node Node ID that has failed
 * @return DSM_SUCCESS on success, error code on failure
 */
int directory_handle_node_failure(page_directory_t *dir, node_id_t failed_node);

/**
 * Reclaim ownership of a page from a failed node
 * Called when a page request to a failed node times out
 * Transfers ownership to the requesting node
 *
 * @param dir Page directory
 * @param page_id Page identifier
 * @param new_owner Node claiming ownership
 * @return DSM_SUCCESS on success, error code on failure
 */
int directory_reclaim_ownership(page_directory_t *dir, page_id_t page_id, node_id_t new_owner);

#endif /* DIRECTORY_H */

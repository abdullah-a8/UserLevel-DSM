/**
 * @file page_table.h
 * @brief Internal page table data structures and functions
 *
 * This file contains the internal implementation of the page table
 * used to track DSM pages, their states, ownership, and metadata.
 */

#ifndef PAGE_TABLE_H
#define PAGE_TABLE_H

#include "dsm/types.h"
#include <pthread.h>

/* ============================ */
/*     Page Entry Structure     */
/* ============================ */

/**
 * Single page table entry
 *
 * Tracks the state and metadata for one page in the DSM system.
 */
typedef struct {
    page_id_t id;              /**< Unique page identifier */
    void *local_addr;          /**< Local virtual address */
    node_id_t owner;           /**< Current owner node */
    page_state_t state;        /**< Current state (INVALID/READ_ONLY/READ_WRITE) */
    uint64_t version;          /**< Version number for consistency */
    bool is_allocated;         /**< True if entry is in use */

    /* For request queuing */
    bool request_pending;      /**< True if page transfer in progress */
    pthread_cond_t ready_cv;   /**< Condition variable for waiting threads */
} page_entry_t;

/* ============================ */
/*     Page Table Structure     */
/* ============================ */

/**
 * Page table for managing all DSM pages
 *
 * This is a simple array-based page table with linear search.
 * For larger systems, could be replaced with hash table.
 */
typedef struct {
    void *base_addr;           /**< Base virtual address of DSM region */
    size_t total_size;         /**< Total size in bytes */
    size_t num_pages;          /**< Number of pages */
    page_entry_t *entries;     /**< Array of page entries */
    pthread_mutex_t lock;      /**< Mutex for thread-safe access */
} page_table_t;

/* ============================ */
/*     Function Declarations    */
/* ============================ */

/**
 * Create a new page table
 *
 * @param base_addr Base address of DSM region
 * @param size Total size of region in bytes
 * @return Pointer to page table, or NULL on failure
 */
page_table_t* page_table_create(void *base_addr, size_t size);

/**
 * Destroy a page table
 *
 * @param table Page table to destroy
 */
void page_table_destroy(page_table_t *table);

/**
 * Look up page entry by virtual address
 *
 * @param table Page table
 * @param addr Virtual address (will be aligned to page boundary)
 * @return Pointer to page entry, or NULL if not found
 */
page_entry_t* page_table_lookup_by_addr(page_table_t *table, void *addr);

/**
 * Look up page entry by page ID
 *
 * @param table Page table
 * @param page_id Page identifier
 * @return Pointer to page entry, or NULL if not found
 */
page_entry_t* page_table_lookup_by_id(page_table_t *table, page_id_t page_id);

/**
 * Set page ownership
 *
 * @param table Page table
 * @param page_id Page identifier
 * @param owner New owner node ID
 * @return DSM_SUCCESS on success, error code on failure
 */
int page_table_set_owner(page_table_t *table, page_id_t page_id, node_id_t owner);

/**
 * Set page state
 *
 * @param table Page table
 * @param page_id Page identifier
 * @param state New page state
 * @return DSM_SUCCESS on success, error code on failure
 */
int page_table_set_state(page_table_t *table, page_id_t page_id, page_state_t state);

/**
 * Get page base address (align address to page boundary)
 *
 * @param addr Any address within a page
 * @return Base address of the page
 */
void* page_get_base_addr(void *addr);

/**
 * Calculate page ID from virtual address
 *
 * @param table Page table
 * @param addr Virtual address
 * @return Page ID
 */
page_id_t page_addr_to_id(page_table_t *table, void *addr);

/**
 * Calculate virtual address from page ID
 *
 * @param table Page table
 * @param page_id Page identifier
 * @return Virtual address of page start
 */
void* page_id_to_addr(page_table_t *table, page_id_t page_id);

#endif /* PAGE_TABLE_H */

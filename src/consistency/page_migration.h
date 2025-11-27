/**
 * @file page_migration.h
 * @brief Page migration and consistency protocol
 *
 * This module implements page fetching for read and write access,
 * including the invalidation protocol.
 */

#ifndef PAGE_MIGRATION_H
#define PAGE_MIGRATION_H

#include "dsm/types.h"

/**
 * Fetch a page for read access
 *
 * This function:
 * 1. Looks up the page owner in the directory
 * 2. Sends a PAGE_REQUEST with READ access
 * 3. Waits for PAGE_REPLY
 * 4. Copies data to local page
 * 5. Sets permission to READ_ONLY
 * 6. Updates directory (adds self to sharers)
 *
 * @param page_id Page identifier
 * @return DSM_SUCCESS on success, error code on failure
 */
int fetch_page_read(page_id_t page_id);

/**
 * Fetch a page for write access
 *
 * This function:
 * 1. Looks up the page owner in the directory
 * 2. Gets list of sharers from directory
 * 3. Sends invalidations to all sharers
 * 4. Waits for all INVALIDATE_ACKs
 * 5. Sends PAGE_REQUEST with WRITE access to owner
 * 6. Waits for PAGE_REPLY
 * 7. Copies data to local page
 * 8. Sets permission to READ_WRITE
 * 9. Updates directory (sets self as owner, clears sharers)
 *
 * @param page_id Page identifier
 * @return DSM_SUCCESS on success, error code on failure
 */
int fetch_page_write(page_id_t page_id);

/**
 * Initialize consistency module
 *
 * @param num_pages Number of pages in DSM
 * @return DSM_SUCCESS on success, error code on failure
 */
int consistency_init(size_t num_pages);

/**
 * Cleanup consistency module
 */
void consistency_cleanup(void);

/**
 * Get global page directory
 *
 * @return Pointer to page directory, or NULL if not initialized
 */
struct page_directory_s* get_page_directory(void);

/**
 * Set global page directory (for failover support)
 *
 * @param dir New page directory pointer
 */
void set_page_directory(struct page_directory_s *dir);

#endif /* PAGE_MIGRATION_H */

/**
 * @file permission.h
 * @brief Page permission management
 */

#ifndef PERMISSION_H
#define PERMISSION_H

#include "dsm/types.h"

/**
 * Set page permission
 */
int set_page_permission(void *addr, page_perm_t perm);

/**
 * Get PROT flags from page_perm_t
 */
int get_prot_flags(page_perm_t perm);

#endif /* PERMISSION_H */

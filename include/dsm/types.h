/**
 * @file types.h
 * @brief Core type definitions for User-Level DSM
 *
 * This file contains all fundamental type definitions used throughout
 * the DSM system including page IDs, node IDs, states, and enums.
 */

#ifndef DSM_TYPES_H
#define DSM_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ============================ */
/*     Configuration            */
/* ============================ */

/** Page size (4KB - standard) */
#define PAGE_SIZE 4096

/** Maximum number of nodes in the cluster */
#define MAX_NODES 16

/** Maximum hostname length */
#define MAX_HOSTNAME_LEN 256

/* ============================ */
/*     Basic Types              */
/* ============================ */

/** Node identifier type */
typedef uint32_t node_id_t;

/** Page identifier type */
typedef uint64_t page_id_t;

/** Lock identifier type */
typedef uint64_t lock_id_t;

/** Barrier identifier type */
typedef uint64_t barrier_id_t;

/* ============================ */
/*     Page States              */
/* ============================ */

/**
 * Page state in the local node
 * State machine:
 *   INVALID -> READ_ONLY (on read fault)
 *   INVALID -> READ_WRITE (on write fault)
 *   READ_ONLY -> READ_WRITE (on write fault)
 *   READ_WRITE -> INVALID (on invalidation)
 *   READ_ONLY -> INVALID (on invalidation)
 */
typedef enum {
    PAGE_STATE_INVALID = 0,   /**< Page not present locally */
    PAGE_STATE_READ_ONLY,     /**< Page present, read-only access */
    PAGE_STATE_READ_WRITE     /**< Page present, read-write access */
} page_state_t;

/* ============================ */
/*     Access Types             */
/* ============================ */

/**
 * Type of memory access that triggered a fault
 */
typedef enum {
    ACCESS_READ = 0,          /**< Read access */
    ACCESS_WRITE              /**< Write access */
} access_type_t;

/* ============================ */
/*     Page Permissions         */
/* ============================ */

/**
 * Memory protection permissions (maps to mprotect)
 */
typedef enum {
    PAGE_PERM_NONE = 0,       /**< No access (PROT_NONE) */
    PAGE_PERM_READ,           /**< Read-only (PROT_READ) */
    PAGE_PERM_READ_WRITE      /**< Read-write (PROT_READ|PROT_WRITE) */
} page_perm_t;

/* ============================ */
/*     Lock States              */
/* ============================ */

/**
 * Distributed lock state
 */
typedef enum {
    LOCK_STATE_FREE = 0,      /**< Lock is free */
    LOCK_STATE_HELD           /**< Lock is held by a node */
} lock_state_t;

/* ============================ */
/*     Return Codes             */
/* ============================ */

/**
 * DSM operation return codes
 */
typedef enum {
    DSM_SUCCESS = 0,          /**< Operation successful */
    DSM_ERROR_INIT = -1,      /**< Initialization error */
    DSM_ERROR_MEMORY = -2,    /**< Memory allocation error */
    DSM_ERROR_NETWORK = -3,   /**< Network error */
    DSM_ERROR_TIMEOUT = -4,   /**< Operation timeout */
    DSM_ERROR_NOT_FOUND = -5, /**< Resource not found */
    DSM_ERROR_INVALID = -6,   /**< Invalid argument */
    DSM_ERROR_BUSY = -7,      /**< Resource busy */
    DSM_ERROR_PERMISSION = -8 /**< Permission denied */
} dsm_error_t;

/* ============================ */
/*     Configuration Struct     */
/* ============================ */

/**
 * DSM configuration parameters
 */
typedef struct {
    node_id_t node_id;               /**< This node's ID */
    uint16_t port;                   /**< Port for server */
    char manager_host[MAX_HOSTNAME_LEN]; /**< Manager hostname */
    uint16_t manager_port;           /**< Manager port */
    int num_nodes;                   /**< Total nodes in cluster */
    bool is_manager;                 /**< True if this is manager node */
    int log_level;                   /**< Logging verbosity (0-4) */
} dsm_config_t;

/* ============================ */
/*     Statistics Struct        */
/* ============================ */

/**
 * DSM runtime statistics
 */
typedef struct {
    uint64_t page_faults;            /**< Total page faults */
    uint64_t read_faults;            /**< Read faults */
    uint64_t write_faults;           /**< Write faults */
    uint64_t pages_fetched;          /**< Pages fetched from remote */
    uint64_t pages_sent;             /**< Pages sent to remote */
    uint64_t invalidations_sent;     /**< Invalidations sent */
    uint64_t invalidations_received; /**< Invalidations received */
    uint64_t network_bytes_sent;     /**< Total bytes sent */
    uint64_t network_bytes_received; /**< Total bytes received */
    uint64_t lock_acquires;          /**< Lock acquisitions */
    uint64_t barrier_waits;          /**< Barrier synchronizations */
} dsm_stats_t;

#endif /* DSM_TYPES_H */

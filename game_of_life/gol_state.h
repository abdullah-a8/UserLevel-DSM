/**
 * @file gol_state.h
 * @brief Game of Life state management
 */

#ifndef GOL_STATE_H
#define GOL_STATE_H

#include <stdint.h>
#include "dsm/dsm.h"
#include "gol_config.h"

/* Cell type - 1 byte per cell (0 = dead, 1 = alive) */
typedef uint8_t cell_t;

/* Maximum nodes supported */
#define MAX_GOL_NODES 4

/**
 * @brief Game of Life runtime state
 * Per-node allocation design: Each node allocates ONLY its partition
 * Other nodes retrieve via SVAS (Single Virtual Address Space)
 */
typedef struct {
    /* Per-node partition pointers (SVAS - same addresses on all nodes) */
    cell_t *partitions_current[MAX_GOL_NODES];  /* Current gen partitions */
    cell_t *partitions_next[MAX_GOL_NODES];     /* Next gen partitions */

    /* Partition metadata (same on all nodes) */
    int partition_start_row[MAX_GOL_NODES];  /* Start row for each partition */
    int partition_num_rows[MAX_GOL_NODES];   /* Number of rows per partition */

    /* This node's partition info */
    int my_node_id;           /* This node's ID */
    int start_row;            /* First row this node computes */
    int end_row;              /* Last row this node computes */
    int num_rows;             /* Number of rows owned by this node */

    /* Grid dimensions */
    int grid_width;
    int grid_height;
    int num_nodes;            /* Total nodes */

    /* DSM synchronization primitives */
    dsm_lock_t *display_lock; /* Lock for terminal output */

    /* Configuration reference */
    const gol_config_t *config;

    /* Statistics */
    uint64_t live_cells_count;      /* Live cells in this partition */
    uint64_t page_faults_start;     /* Page faults at start */
    uint64_t page_faults_end;       /* Page faults at end */
} gol_state_t;

/**
 * @brief Initialize Game of Life state
 * @param state State structure to initialize
 * @param config Configuration
 * @return DSM_SUCCESS on success, error code otherwise
 */
int gol_state_init(gol_state_t *state, const gol_config_t *config);

/**
 * @brief Cleanup Game of Life state
 * Following test_multinode.c pattern: barriers before cleanup
 * @param state State structure to cleanup
 */
void gol_state_cleanup(gol_state_t *state);

/**
 * @brief Calculate partition boundaries for this node
 * Distributes rows evenly across nodes
 * @param state State structure
 */
void gol_calculate_partition(gol_state_t *state);

/**
 * @brief Allocate this node's partition in DSM
 * Per-node allocation: EACH node allocates its own partition
 * @param state State structure
 * @return DSM_SUCCESS on success, error code otherwise
 */
int gol_allocate_partition(gol_state_t *state);

/**
 * @brief Retrieve other nodes' partitions via SVAS
 * After allocation, retrieve pointers to other nodes' partitions
 * @param state State structure
 * @return DSM_SUCCESS on success, error code otherwise
 */
int gol_retrieve_partitions(gol_state_t *state);

/**
 * @brief Get cell address for any (row, col) in the grid
 * Maps to correct partition, handling per-node allocation
 * @param state State structure
 * @param grid Which grid (0=current, 1=next)
 * @param row Row index
 * @param col Column index
 * @return Pointer to cell, or NULL if invalid
 */
cell_t* gol_get_cell(const gol_state_t *state, int grid, int row, int col);

/**
 * @brief Set cell value for any (row, col) in the grid
 * Maps to correct partition
 * @param state State structure
 * @param grid Which grid (0=current, 1=next)
 * @param row Row index
 * @param col Column index
 * @param value Cell value to set
 * @return DSM_SUCCESS on success, error code otherwise
 */
int gol_set_cell(const gol_state_t *state, int grid, int row, int col, cell_t value);

#endif /* GOL_STATE_H */

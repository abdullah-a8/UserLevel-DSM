/**
 * @file gol_state.c
 * @brief Game of Life state management - Per-node allocation design
 * Each node allocates ONLY its partition to comply with SWMR protocol
 */

#include "gol_state.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int gol_state_init(gol_state_t *state, const gol_config_t *config) {
    if (!state || !config) {
        fprintf(stderr, "[Node %d] NULL state or config\n", config ? config->node_id : -1);
        return DSM_ERROR_INVALID;
    }

    /* Initialize state structure */
    memset(state, 0, sizeof(gol_state_t));
    state->config = config;
    state->grid_width = config->grid_width;
    state->grid_height = config->grid_height;
    state->num_nodes = config->num_nodes;
    state->my_node_id = config->node_id;

    /* Calculate partition boundaries for ALL nodes */
    gol_calculate_partition(state);

    printf("[Node %d] State initialized: my partition rows [%d, %d), %d rows total\n",
           config->node_id, state->start_row, state->end_row, state->num_rows);

    return DSM_SUCCESS;
}

void gol_calculate_partition(gol_state_t *state) {
    if (!state || !state->config) {
        return;
    }

    const gol_config_t *config = state->config;
    const int height = state->grid_height;
    const int num_nodes = config->num_nodes;

    /* Calculate partition boundaries for ALL nodes (metadata shared) */
    int rows_per_node = height / num_nodes;
    int remainder = height % num_nodes;

    int current_row = 0;
    for (int node = 0; node < num_nodes; node++) {
        state->partition_start_row[node] = current_row;

        /* Distribute remainder rows to first nodes */
        int rows_for_this_node = rows_per_node + (node < remainder ? 1 : 0);
        state->partition_num_rows[node] = rows_for_this_node;

        current_row += rows_for_this_node;

        printf("[Node %d] Partition %d: rows [%d, %d), %d rows\n",
               config->node_id, node,
               state->partition_start_row[node],
               state->partition_start_row[node] + state->partition_num_rows[node],
               state->partition_num_rows[node]);
    }

    /* Set this node's partition info */
    state->start_row = state->partition_start_row[config->node_id];
    state->end_row = state->start_row + state->partition_num_rows[config->node_id];
    state->num_rows = state->partition_num_rows[config->node_id];
}

int gol_allocate_partition(gol_state_t *state) {
    if (!state || !state->config) {
        fprintf(stderr, "NULL state or config\n");
        return DSM_ERROR_INVALID;
    }

    const gol_config_t *config = state->config;
    const int my_node = config->node_id;

    /* Calculate this node's partition size */
    size_t partition_size = state->num_rows * state->grid_width * sizeof(cell_t);

    printf("[Node %d] Allocating partition: %d rows × %d cols = %zu bytes\n",
           my_node, state->num_rows, state->grid_width, partition_size);

    /* Allocate current generation partition */
    state->partitions_current[my_node] = (cell_t*)dsm_malloc(partition_size);
    if (!state->partitions_current[my_node]) {
        fprintf(stderr, "[Node %d] Failed to allocate partitions_current[%d] (%zu bytes)\n",
                my_node, my_node, partition_size);
        return DSM_ERROR_MEMORY;
    }

    printf("[Node %d] Allocated partitions_current[%d] at %p (%zu bytes)\n",
           my_node, my_node, (void*)state->partitions_current[my_node], partition_size);

    /* Allocate next generation partition */
    state->partitions_next[my_node] = (cell_t*)dsm_malloc(partition_size);
    if (!state->partitions_next[my_node]) {
        fprintf(stderr, "[Node %d] Failed to allocate partitions_next[%d]\n",
                my_node, my_node);
        dsm_free(state->partitions_current[my_node]);
        state->partitions_current[my_node] = NULL;
        return DSM_ERROR_MEMORY;
    }

    printf("[Node %d] Allocated partitions_next[%d] at %p (%zu bytes)\n",
           my_node, my_node, (void*)state->partitions_next[my_node], partition_size);

    /* Initialize partition to zero */
    memset(state->partitions_current[my_node], 0, partition_size);
    memset(state->partitions_next[my_node], 0, partition_size);

    printf("[Node %d] Partition initialized to zero\n", my_node);

    return DSM_SUCCESS;
}

int gol_retrieve_partitions(gol_state_t *state) {
    if (!state || !state->config) {
        fprintf(stderr, "NULL state or config\n");
        return DSM_ERROR_INVALID;
    }

    const gol_config_t *config = state->config;
    const int my_node = config->node_id;
    const int num_nodes = config->num_nodes;

    printf("[Node %d] Retrieving other nodes' partitions via SVAS...\n", my_node);

    /* Calculate allocation index for each node
     * Each node allocates 2 partitions (current and next)
     * Node 0: allocations 0, 1
     * Node 1: allocations 2, 3
     * Node N: allocations 2*N, 2*N+1
     */

    for (int node = 0; node < num_nodes; node++) {
        if (node == my_node) {
            /* Already have our own partition */
            printf("[Node %d] Partition %d: Own allocation (already set)\n",
                   my_node, node);
            continue;
        }

        int alloc_idx_current = node * 2;
        int alloc_idx_next = node * 2 + 1;

        /* Retrieve remote partition for current generation */
        state->partitions_current[node] = (cell_t*)dsm_get_allocation(alloc_idx_current);
        if (!state->partitions_current[node]) {
            fprintf(stderr, "[Node %d] Failed to retrieve partitions_current[%d] (allocation %d)\n",
                    my_node, node, alloc_idx_current);
            return DSM_ERROR_NOT_FOUND;
        }

        printf("[Node %d] Retrieved partitions_current[%d] at %p (allocation %d)\n",
               my_node, node, (void*)state->partitions_current[node], alloc_idx_current);

        /* Retrieve remote partition for next generation */
        state->partitions_next[node] = (cell_t*)dsm_get_allocation(alloc_idx_next);
        if (!state->partitions_next[node]) {
            fprintf(stderr, "[Node %d] Failed to retrieve partitions_next[%d] (allocation %d)\n",
                    my_node, node, alloc_idx_next);
            return DSM_ERROR_NOT_FOUND;
        }

        printf("[Node %d] Retrieved partitions_next[%d] at %p (allocation %d)\n",
               my_node, node, (void*)state->partitions_next[node], alloc_idx_next);
    }

    printf("[Node %d] All partitions retrieved successfully\n", my_node);
    return DSM_SUCCESS;
}

cell_t* gol_get_cell(const gol_state_t *state, int grid, int row, int col) {
    if (!state) {
        return NULL;
    }

    /* Validate row and column */
    if (row < 0 || row >= state->grid_height || col < 0 || col >= state->grid_width) {
        return NULL;
    }

    /* Determine which node owns this row */
    int owner_node = -1;
    int local_row = -1;

    for (int node = 0; node < state->num_nodes; node++) {
        int start = state->partition_start_row[node];
        int num_rows = state->partition_num_rows[node];

        if (row >= start && row < start + num_rows) {
            owner_node = node;
            local_row = row - start;
            break;
        }
    }

    if (owner_node == -1 || local_row == -1) {
        fprintf(stderr, "[Node %d] Failed to map row %d to any partition\n",
                state->my_node_id, row);
        return NULL;
    }

    /* Get pointer to the partition */
    cell_t *partition = (grid == 0) ? state->partitions_current[owner_node]
                                    : state->partitions_next[owner_node];

    if (!partition) {
        fprintf(stderr, "[Node %d] Partition %d (grid %d) is NULL\n",
                state->my_node_id, owner_node, grid);
        return NULL;
    }

    /* Calculate offset within partition */
    size_t offset = local_row * state->grid_width + col;

    return &partition[offset];
}

int gol_set_cell(const gol_state_t *state, int grid, int row, int col, cell_t value) {
    cell_t *cell = gol_get_cell(state, grid, row, col);
    if (!cell) {
        return DSM_ERROR_INVALID;
    }

    *cell = value;
    return DSM_SUCCESS;
}

void gol_state_cleanup(gol_state_t *state) {
    if (!state || !state->config) {
        return;
    }

    const gol_config_t *config = state->config;
    const int my_node = config->node_id;

    printf("[Node %d] Cleaning up state...\n", my_node);

    /* Cleanup display lock */
    if (state->display_lock) {
        dsm_lock_destroy(state->display_lock);
        state->display_lock = NULL;
    }

    /* Free only OUR partitions (we own them) */
    if (state->partitions_current[my_node]) {
        dsm_free(state->partitions_current[my_node]);
        state->partitions_current[my_node] = NULL;
    }

    if (state->partitions_next[my_node]) {
        dsm_free(state->partitions_next[my_node]);
        state->partitions_next[my_node] = NULL;
    }

    /* Don't free other nodes' partitions - they own them! */

    printf("[Node %d] State cleanup complete\n", my_node);
}

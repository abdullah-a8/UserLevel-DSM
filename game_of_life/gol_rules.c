/**
 * @file gol_rules.c
 * @brief Conway's Game of Life rules - Partition-aware implementation
 */

#include "gol_rules.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/**
 * Count live neighbors with toroidal wrapping
 * CRITICAL: Uses gol_get_cell() which may trigger DSM page faults
 */
int count_neighbors(const gol_state_t *state, int row, int col) {
    int count = 0;
    const int width = state->grid_width;
    const int height = state->grid_height;

    /* Check all 8 neighbors */
    for (int dr = -1; dr <= 1; dr++) {
        for (int dc = -1; dc <= 1; dc++) {
            if (dr == 0 && dc == 0) {
                continue;  /* Skip the cell itself */
            }

            /* Toroidal wrapping: wrap around edges */
            int neighbor_row = (row + dr + height) % height;
            int neighbor_col = (col + dc + width) % width;

            /* CRITICAL: This access may trigger a page fault if neighbor
               is on another node's partition - this is INTENTIONAL and
               demonstrates DSM page migration */
            cell_t *cell = gol_get_cell(state, 0, neighbor_row, neighbor_col);
            if (cell) {
                count += *cell;
            }
        }
    }

    return count;
}

/**
 * Apply Conway's Game of Life rules
 */
cell_t apply_rules(cell_t current_state, int neighbor_count) {
    if (current_state == 1) {
        /* Live cell */
        if (neighbor_count == 2 || neighbor_count == 3) {
            return 1;  /* Survives */
        } else {
            return 0;  /* Dies (underpopulation or overpopulation) */
        }
    } else {
        /* Dead cell */
        if (neighbor_count == 3) {
            return 1;  /* Becomes alive (reproduction) */
        } else {
            return 0;  /* Stays dead */
        }
    }
}

/**
 * Compute next generation for this node's partition
 */
uint64_t compute_generation(gol_state_t *state) {
    if (!state) {
        fprintf(stderr, "Invalid state in compute_generation\n");
        return 0;
    }

    const int width = state->grid_width;
    uint64_t live_count = 0;

    /* Each node computes only its partition: [start_row, end_row) */
    for (int row = state->start_row; row < state->end_row; row++) {
        for (int col = 0; col < width; col++) {
            /* Count neighbors - may trigger page faults at boundaries */
            int neighbors = count_neighbors(state, row, col);

            /* Get current cell state */
            cell_t *current_cell = gol_get_cell(state, 0, row, col);
            if (!current_cell) {
                fprintf(stderr, "[Node %d] Failed to get cell (%d, %d)\n",
                        state->my_node_id, row, col);
                continue;
            }

            /* Apply rules and write to next generation */
            cell_t next = apply_rules(*current_cell, neighbors);

            /* CRITICAL: Write only to OUR partition of grid_next
               This does NOT violate SWMR because we own this partition */
            cell_t *next_cell = gol_get_cell(state, 1, row, col);
            if (next_cell) {
                *next_cell = next;
                if (next == 1) {
                    live_count++;
                }
            }
        }
    }

    return live_count;
}

/**
 * Count live cells in this node's partition only
 * Note: grid parameter is unused (kept for API compatibility)
 */
uint64_t count_live_cells(const cell_t *grid, const gol_state_t *state) {
    (void)grid;  /* Unused - we use gol_get_cell instead */

    if (!state) {
        return 0;
    }

    uint64_t count = 0;
    const int width = state->grid_width;

    /* Count only in this node's partition */
    for (int row = state->start_row; row < state->end_row; row++) {
        for (int col = 0; col < width; col++) {
            const cell_t *cell = gol_get_cell(state, 0, row, col);
            if (cell) {
                count += *cell;
            }
        }
    }

    return count;
}

/**
 * Initialize glider pattern
 */
void init_glider(gol_state_t *state, int x, int y) {
    if (!state) return;

    const int width = state->grid_width;
    const int height = state->grid_height;

    /* Glider pattern:
       .#.
       ..#
       ###
    */
    int pattern[5][2] = {
        {0, 1},  /* Row 0, Col 1 */
        {1, 2},  /* Row 1, Col 2 */
        {2, 0},  /* Row 2, Col 0 */
        {2, 1},  /* Row 2, Col 1 */
        {2, 2}   /* Row 2, Col 2 */
    };

    for (int i = 0; i < 5; i++) {
        int row = (y + pattern[i][0]) % height;
        int col = (x + pattern[i][1]) % width;
        gol_set_cell(state, 0, row, col, 1);
    }

    printf("[Node %d] Initialized glider at (%d, %d)\n", state->my_node_id, x, y);
}

/**
 * Initialize R-pentomino pattern
 */
void init_rpentomino(gol_state_t *state, int x, int y) {
    if (!state) return;

    const int width = state->grid_width;
    const int height = state->grid_height;

    /* R-pentomino pattern:
       .##
       ##.
       .#.
    */
    int pattern[5][2] = {
        {0, 1},  /* Row 0, Col 1 */
        {0, 2},  /* Row 0, Col 2 */
        {1, 0},  /* Row 1, Col 0 */
        {1, 1},  /* Row 1, Col 1 */
        {2, 1}   /* Row 2, Col 1 */
    };

    for (int i = 0; i < 5; i++) {
        int row = (y + pattern[i][0]) % height;
        int col = (x + pattern[i][1]) % width;
        gol_set_cell(state, 0, row, col, 1);
    }

    printf("[Node %d] Initialized R-pentomino at (%d, %d)\n", state->my_node_id, x, y);
}

/**
 * Initialize blinker pattern (period-2 oscillator)
 */
void init_blinker(gol_state_t *state, int x, int y) {
    if (!state) return;

    const int width = state->grid_width;
    const int height = state->grid_height;

    /* Blinker pattern (horizontal):
       ###
    */
    for (int i = 0; i < 3; i++) {
        int row = y % height;
        int col = (x + i) % width;
        gol_set_cell(state, 0, row, col, 1);
    }

    printf("[Node %d] Initialized blinker at (%d, %d)\n", state->my_node_id, x, y);
}

/**
 * Initialize random pattern in this node's partition only
 */
void init_random_partition(gol_state_t *state, float density, unsigned int seed) {
    if (!state) return;

    srand(seed);

    int live_count = 0;
    const int width = state->grid_width;

    /* Initialize only this node's partition */
    for (int row = state->start_row; row < state->end_row; row++) {
        for (int col = 0; col < width; col++) {
            float r = (float)rand() / (float)RAND_MAX;
            if (r < density) {
                gol_set_cell(state, 0, row, col, 1);
                live_count++;
            } else {
                gol_set_cell(state, 0, row, col, 0);
            }
        }
    }

    printf("[Node %d] Initialized random pattern in partition (density=%.2f, seed=%u, live=%d)\n",
           state->my_node_id, density, seed, live_count);
}

/**
 * Initialize pattern based on configuration
 * CRITICAL: ALL nodes participate in initialization
 */
void initialize_pattern(gol_state_t *state) {
    if (!state || !state->config) {
        fprintf(stderr, "Invalid state in initialize_pattern\n");
        return;
    }

    const gol_config_t *config = state->config;

    printf("[Node %d] Initializing pattern type %d...\n",
           config->node_id, config->pattern);

    /* Initialize center point (only Node 0 for structured patterns) */
    int center_x = state->grid_width / 2;
    int center_y = state->grid_height / 2;

    switch (config->pattern) {
        case PATTERN_GLIDER:
            /* Only Node 0 initializes structured patterns */
            if (config->node_id == 0) {
                init_glider(state, center_x, center_y);
            }
            break;

        case PATTERN_RPENTOMINO:
            if (config->node_id == 0) {
                init_rpentomino(state, center_x, center_y);
            }
            break;

        case PATTERN_BLINKER:
            if (config->node_id == 0) {
                init_blinker(state, center_x, center_y);
            }
            break;

        case PATTERN_RANDOM:
            /* Each node initializes its own partition independently */
            init_random_partition(state, config->random_density,
                                (unsigned int)time(NULL) + config->node_id);
            break;

        default:
            fprintf(stderr, "[Node %d] Unknown pattern type: %d\n",
                   config->node_id, config->pattern);
            break;
    }

    /* Count initial live cells in this partition */
    uint64_t live_cells = count_live_cells(NULL, state);
    printf("[Node %d] Pattern initialized with %lu live cells in partition\n",
           config->node_id, live_cells);
}

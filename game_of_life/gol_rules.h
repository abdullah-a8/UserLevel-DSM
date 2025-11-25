/**
 * @file gol_rules.h
 * @brief Conway's Game of Life rules and computation
 */

#ifndef GOL_RULES_H
#define GOL_RULES_H

#include "gol_state.h"
#include "gol_config.h"

/**
 * @brief Count live neighbors for a cell (8-neighborhood with toroidal wrapping)
 *
 * This function will access cells that may be on other nodes' partitions,
 * triggering DSM page faults as designed.
 *
 * @param state Game state
 * @param row Row index
 * @param col Column index
 * @return Number of live neighbors (0-8)
 */
int count_neighbors(const gol_state_t *state, int row, int col);

/**
 * @brief Apply Conway's Game of Life rules
 *
 * Rules:
 * - Any live cell with 2-3 live neighbors survives
 * - Any dead cell with exactly 3 live neighbors becomes alive
 * - All other cells die or remain dead
 *
 * @param current_state Current cell state (0=dead, 1=alive)
 * @param neighbor_count Number of live neighbors
 * @return Next cell state (0=dead, 1=alive)
 */
cell_t apply_rules(cell_t current_state, int neighbor_count);

/**
 * @brief Compute next generation for this node's partition
 *
 * Each node computes only its assigned rows [start_row, end_row).
 * Boundary row accesses will trigger DSM page faults.
 *
 * @param state Game state
 * @return Number of live cells in this partition
 */
uint64_t compute_generation(gol_state_t *state);

/**
 * @brief Count live cells in this node's partition
 *
 * @param grid Grid to count
 * @param state Game state (for partition boundaries)
 * @return Number of live cells
 */
uint64_t count_live_cells(const cell_t *grid, const gol_state_t *state);

/**
 * @brief Initialize grid with a glider pattern
 *
 * Glider is a small spaceship that moves diagonally:
 *   .#.
 *   ..#
 *   ###
 *
 * @param state Game state
 * @param x Starting X position
 * @param y Starting Y position
 */
void init_glider(gol_state_t *state, int x, int y);

/**
 * @brief Initialize grid with R-pentomino pattern
 *
 * R-pentomino is a methuselah that stabilizes after 1103 generations:
 *   .##
 *   ##.
 *   .#.
 *
 * @param state Game state
 * @param x Starting X position
 * @param y Starting Y position
 */
void init_rpentomino(gol_state_t *state, int x, int y);

/**
 * @brief Initialize grid with blinker pattern
 *
 * Blinker is a period-2 oscillator:
 *   ###  (horizontal)
 *   or
 *   #    (vertical)
 *   #
 *   #
 *
 * @param state Game state
 * @param x Starting X position
 * @param y Starting Y position
 */
void init_blinker(gol_state_t *state, int x, int y);

/**
 * @brief Initialize random pattern in this node's partition only
 *
 * @param state Game state
 * @param density Probability of cell being alive (0.0 - 1.0)
 * @param seed Random seed (use node_id for different patterns per node)
 */
void init_random_partition(gol_state_t *state, float density, unsigned int seed);

/**
 * @brief Initialize grid based on configuration
 *
 * Only Node 0 should call this to initialize the pattern.
 *
 * @param state Game state
 */
void initialize_pattern(gol_state_t *state);

#endif /* GOL_RULES_H */

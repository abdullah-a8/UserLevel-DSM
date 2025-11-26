"""Game of Life rules implementation using NumPy for efficiency."""

import numpy as np
from typing import Tuple

from dsm_visualizer.models.grid_state import GridState


class GameOfLifeRules:
    """
    Implements Conway's Game of Life rules.

    Rules:
    1. Any live cell with 2 or 3 live neighbors survives.
    2. Any dead cell with exactly 3 live neighbors becomes alive.
    3. All other cells die or stay dead.
    """

    @staticmethod
    def count_neighbors(grid: GridState, row: int, col: int) -> int:
        """
        Count the number of live neighbors for a cell.

        Args:
            grid: The grid state.
            row: Row of the cell.
            col: Column of the cell.

        Returns:
            Number of live neighbors (0-8).
        """
        count = 0
        for dr in [-1, 0, 1]:
            for dc in [-1, 0, 1]:
                if dr == 0 and dc == 0:
                    continue
                nr, nc = row + dr, col + dc
                # Handle boundary - cells outside grid are dead
                if 0 <= nr < grid.height and 0 <= nc < grid.width:
                    count += grid.cells[nr, nc]
        return count

    @staticmethod
    def compute_next_generation(grid: GridState) -> GridState:
        """
        Compute the next generation using Game of Life rules.

        Uses NumPy convolution for efficient neighbor counting.

        Args:
            grid: Current grid state.

        Returns:
            New GridState with the next generation.
        """
        # Create kernel for counting neighbors
        # This counts all 8 neighbors (not the center cell)
        cells = grid.cells.astype(np.int32)

        # Pad the grid with zeros for boundary handling
        padded = np.pad(cells, 1, mode="constant", constant_values=0)

        # Count neighbors using slicing (faster than convolution for small kernels)
        neighbors = (
            padded[:-2, :-2]
            + padded[:-2, 1:-1]
            + padded[:-2, 2:]  # Top row
            + padded[1:-1, :-2]
            + padded[1:-1, 2:]  # Middle row (no center)
            + padded[2:, :-2]
            + padded[2:, 1:-1]
            + padded[2:, 2:]  # Bottom row
        )

        # Apply Game of Life rules:
        # - Live cell with 2-3 neighbors survives
        # - Dead cell with exactly 3 neighbors becomes alive
        new_cells = np.where(
            (cells == 1) & ((neighbors == 2) | (neighbors == 3)),  # Survival
            1,
            np.where((cells == 0) & (neighbors == 3), 1, 0),  # Birth  # Death/stay dead
        ).astype(np.uint8)

        # Create new grid with updated cells
        new_grid = GridState(grid.width, grid.height, grid.num_nodes)
        new_grid.cells = new_cells

        return new_grid

    @staticmethod
    def get_boundary_accesses(
        grid: GridState, node_id: int
    ) -> list[Tuple[int, int, int]]:
        """
        Get the boundary row accesses that would cause page faults.

        When a node computes its partition, it needs to read neighbor rows
        from adjacent partitions. This returns the (row, neighbor_row, neighbor_owner)
        tuples that represent cross-partition accesses.

        Args:
            grid: The grid state.
            node_id: The node computing its partition.

        Returns:
            List of (computing_row, accessed_row, owner_of_accessed_row) tuples.
        """
        accesses = []
        start, end = grid.partition_boundaries[node_id]

        # First row of partition needs to access row above (previous partition)
        if start > 0:
            prev_owner = grid.get_owner(start - 1)
            accesses.append((start, start - 1, prev_owner))

        # Last row of partition needs to access row below (next partition)
        if end < grid.height:
            next_owner = grid.get_owner(end)
            accesses.append((end - 1, end, next_owner))

        return accesses

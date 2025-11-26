"""Grid state representation for Game of Life."""

from dataclasses import dataclass, field
from typing import List, Tuple
import numpy as np


@dataclass
class GridState:
    """
    Represents the state of a Game of Life grid with DSM partitioning information.

    The grid is divided into horizontal partitions, one per node. Each node
    owns a contiguous range of rows.
    """

    width: int
    height: int
    num_nodes: int
    cells: np.ndarray = field(default=None, repr=False)

    def __post_init__(self):
        """Initialize cells array and calculate partitions."""
        if self.cells is None:
            self.cells = np.zeros((self.height, self.width), dtype=np.uint8)
        self._partition_boundaries = self._calculate_partitions()

    def _calculate_partitions(self) -> List[Tuple[int, int]]:
        """
        Calculate partition boundaries for each node.

        Returns:
            List of (start_row, end_row) tuples for each node.
            end_row is exclusive.
        """
        rows_per_node = self.height // self.num_nodes
        remainder = self.height % self.num_nodes

        partitions = []
        current_row = 0

        for i in range(self.num_nodes):
            # Distribute remainder rows to first nodes
            extra = 1 if i < remainder else 0
            num_rows = rows_per_node + extra
            end_row = current_row + num_rows
            partitions.append((current_row, end_row))
            current_row = end_row

        return partitions

    @property
    def partition_boundaries(self) -> List[Tuple[int, int]]:
        """Get partition boundaries as [(start_row, end_row), ...] for each node."""
        return self._partition_boundaries

    def get_owner(self, row: int) -> int:
        """
        Get the node ID that owns a given row.

        Args:
            row: The row index to check.

        Returns:
            Node ID (0-indexed) that owns this row, or -1 if invalid.
        """
        if row < 0 or row >= self.height:
            return -1

        for node_id, (start, end) in enumerate(self._partition_boundaries):
            if start <= row < end:
                return node_id
        return -1

    def is_boundary_row(self, row: int) -> bool:
        """
        Check if a row is at a partition boundary.

        Boundary rows are where page faults occur, as they need to access
        neighbor cells from adjacent partitions.

        Args:
            row: The row index to check.

        Returns:
            True if the row is at a partition boundary.
        """
        for start, end in self._partition_boundaries:
            # First row of partition (except first partition) needs previous node's data
            if start > 0 and row == start:
                return True
            # Last row of partition (except last partition) needs next node's data
            if end < self.height and row == end - 1:
                return True
        return False

    def get_cell(self, row: int, col: int) -> int:
        """Get cell value at (row, col). Returns 0 for out-of-bounds."""
        if 0 <= row < self.height and 0 <= col < self.width:
            return self.cells[row, col]
        return 0

    def set_cell(self, row: int, col: int, value: int) -> None:
        """Set cell value at (row, col)."""
        if 0 <= row < self.height and 0 <= col < self.width:
            self.cells[row, col] = value

    def count_live_cells(self) -> int:
        """Count total number of live cells."""
        return int(np.sum(self.cells))

    def count_live_cells_in_partition(self, node_id: int) -> int:
        """Count live cells in a specific node's partition."""
        if node_id < 0 or node_id >= self.num_nodes:
            return 0
        start, end = self._partition_boundaries[node_id]
        return int(np.sum(self.cells[start:end, :]))

    def clear(self) -> None:
        """Clear all cells (set to dead)."""
        self.cells.fill(0)

    def randomize(self, density: float = 0.3) -> None:
        """
        Randomize grid with given density of live cells.

        Args:
            density: Probability of each cell being alive (0.0 to 1.0).
        """
        self.cells = (np.random.random((self.height, self.width)) < density).astype(
            np.uint8
        )

    def copy(self) -> "GridState":
        """Create a deep copy of this grid state."""
        new_grid = GridState(self.width, self.height, self.num_nodes)
        new_grid.cells = self.cells.copy()
        return new_grid

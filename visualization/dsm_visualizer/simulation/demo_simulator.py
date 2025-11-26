"""Demo simulator that runs Game of Life with simulated DSM statistics."""

from typing import Callable, Optional

from dsm_visualizer.config import VisualizerConfig
from dsm_visualizer.models.grid_state import GridState
from dsm_visualizer.models.dsm_stats import DSMStats, NodeStats
from dsm_visualizer.simulation.gol_rules import GameOfLifeRules


class DemoSimulator:
    """
    Standalone demo simulator for Game of Life with DSM-like behavior.

    This simulates what would happen in a real distributed DSM system:
    - Each node computes its own partition
    - Accessing boundary rows triggers "page faults"
    - Statistics are tracked per-node

    No actual network or multi-process required - perfect for presentations.
    """

    def __init__(self, config: VisualizerConfig):
        """
        Initialize the demo simulator.

        Args:
            config: Visualizer configuration.
        """
        self.config = config
        self.grid = GridState(config.grid_width, config.grid_height, config.num_nodes)
        self.stats = DSMStats()
        self.generation = 0

        # Initialize per-node stats
        for i in range(config.num_nodes):
            self.stats.set_node(i, NodeStats(node_id=i))

        # Callback for page fault animations
        self._fault_callback: Optional[Callable[[int, int], None]] = None

        # Simulated network overhead per page (bytes)
        self._page_size = 4096
        self._message_overhead = 64  # Protocol header size

    def set_fault_callback(self, callback: Callable[[int, int], None]) -> None:
        """
        Set a callback to be called when a simulated page fault occurs.

        Args:
            callback: Function that takes (row, owner_node_id) parameters.
        """
        self._fault_callback = callback

    def initialize_random(self, density: float = 0.3) -> None:
        """Initialize grid with random cells."""
        self.grid.randomize(density)
        self.generation = 0
        # Reset stats
        for i in range(self.config.num_nodes):
            self.stats.set_node(i, NodeStats(node_id=i))

    def initialize_pattern(self, pattern: str = "glider_gun") -> None:
        """
        Initialize grid with a predefined pattern.

        Args:
            pattern: Pattern name ("glider", "glider_gun", "random", "acorn", "rpentomino")
        """
        self.grid.clear()
        self.generation = 0

        # Reset stats
        for i in range(self.config.num_nodes):
            self.stats.set_node(i, NodeStats(node_id=i))

        center_row = self.grid.height // 2
        center_col = self.grid.width // 2

        if pattern == "glider":
            self._place_glider(center_row - 10, center_col - 10)
        elif pattern == "glider_gun":
            self._place_gosper_glider_gun(5, 2)
        elif pattern == "acorn":
            self._place_acorn(center_row, center_col)
        elif pattern == "rpentomino":
            self._place_rpentomino(center_row, center_col)
        elif pattern == "random":
            self.grid.randomize(0.3)
        else:
            # Default to random
            self.grid.randomize(0.3)

    def _place_glider(self, row: int, col: int) -> None:
        """Place a glider pattern at the specified position."""
        pattern = [
            [0, 1, 0],
            [0, 0, 1],
            [1, 1, 1],
        ]
        self._place_pattern(pattern, row, col)

    def _place_gosper_glider_gun(self, row: int, col: int) -> None:
        """Place a Gosper Glider Gun pattern."""
        # Gosper Glider Gun - creates gliders continuously
        pattern = [
            [
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                1,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
            ],
            [
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                1,
                0,
                1,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
            ],
            [
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                1,
                1,
                0,
                0,
                0,
                0,
                0,
                0,
                1,
                1,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                1,
                1,
            ],
            [
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                1,
                0,
                0,
                0,
                1,
                0,
                0,
                0,
                0,
                1,
                1,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                1,
                1,
            ],
            [
                1,
                1,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                1,
                0,
                0,
                0,
                0,
                0,
                1,
                0,
                0,
                0,
                1,
                1,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
            ],
            [
                1,
                1,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                1,
                0,
                0,
                0,
                1,
                0,
                1,
                1,
                0,
                0,
                0,
                0,
                1,
                0,
                1,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
            ],
            [
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                1,
                0,
                0,
                0,
                0,
                0,
                1,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                1,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
            ],
            [
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                1,
                0,
                0,
                0,
                1,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
            ],
            [
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                1,
                1,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
            ],
        ]
        self._place_pattern(pattern, row, col)

    def _place_acorn(self, row: int, col: int) -> None:
        """Place an Acorn pattern - takes 5206 generations to stabilize."""
        pattern = [
            [0, 1, 0, 0, 0, 0, 0],
            [0, 0, 0, 1, 0, 0, 0],
            [1, 1, 0, 0, 1, 1, 1],
        ]
        self._place_pattern(pattern, row - 1, col - 3)

    def _place_rpentomino(self, row: int, col: int) -> None:
        """Place an R-pentomino pattern - chaotic growth."""
        pattern = [
            [0, 1, 1],
            [1, 1, 0],
            [0, 1, 0],
        ]
        self._place_pattern(pattern, row - 1, col - 1)

    def _place_pattern(self, pattern: list[list[int]], row: int, col: int) -> None:
        """Place a pattern at the specified position."""
        for r, row_data in enumerate(pattern):
            for c, cell in enumerate(row_data):
                gr, gc = row + r, col + c
                if 0 <= gr < self.grid.height and 0 <= gc < self.grid.width:
                    self.grid.cells[gr, gc] = cell

    def step(self) -> None:
        """
        Compute one generation and simulate DSM activity.

        This simulates what happens in the real DSM:
        1. Each node computes its partition
        2. Accessing boundary rows triggers page faults
        3. Statistics are updated
        """
        # Simulate page faults at boundaries for each node
        self._simulate_boundary_faults()

        # Compute next generation
        self.grid = GameOfLifeRules.compute_next_generation(self.grid)

        # Simulate barrier synchronization
        for node_id in range(self.config.num_nodes):
            node_stats = self.stats.get_node(node_id)
            node_stats.barrier_waits += 2  # compute barrier + swap barrier

        self.generation += 1
        self.stats.generation = self.generation

    def _simulate_boundary_faults(self) -> None:
        """
        Simulate page faults that would occur at partition boundaries.

        In real DSM:
        - When Node 0 computes row N-1 (last row), it reads row N (Node 1's first row)
        - This triggers a page fault, fetching the page from Node 1
        - Network traffic is generated
        """
        for node_id in range(self.config.num_nodes):
            node_stats = self.stats.get_node(node_id)
            accesses = GameOfLifeRules.get_boundary_accesses(self.grid, node_id)

            for computing_row, accessed_row, owner in accesses:
                # This is a read fault - accessing another node's data
                node_stats.read_faults += 1
                node_stats.page_faults += 1
                node_stats.pages_fetched += 1

                # Simulate network traffic
                # Request message sent to owner
                node_stats.bytes_sent += self._message_overhead
                # Page data received from owner
                node_stats.bytes_received += self._page_size + self._message_overhead

                # Update the owner's stats (they sent the page)
                owner_stats = self.stats.get_node(owner)
                owner_stats.pages_sent += 1
                owner_stats.bytes_sent += self._page_size + self._message_overhead
                owner_stats.bytes_received += self._message_overhead

                # Trigger fault animation callback
                if self._fault_callback:
                    self._fault_callback(accessed_row, owner)

    def get_grid(self) -> GridState:
        """Get the current grid state."""
        return self.grid

    def get_stats(self) -> DSMStats:
        """Get the current statistics."""
        return self.stats

    def get_generation(self) -> int:
        """Get the current generation number."""
        return self.generation

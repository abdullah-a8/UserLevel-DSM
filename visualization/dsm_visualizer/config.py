"""Configuration and constants for the DSM visualizer."""

from dataclasses import dataclass, field
from typing import Tuple

# ==============================================================================
# Color Scheme (Colorblind-friendly)
# ==============================================================================

# Node colors - each node gets a distinct color for its partition
NODE_COLORS: list[Tuple[int, int, int]] = [
    (66, 133, 244),  # Node 0: Blue
    (52, 168, 83),  # Node 1: Green
    (251, 188, 4),  # Node 2: Yellow
    (234, 67, 53),  # Node 3: Red
]

# Alive cell colors (brighter versions of node colors)
NODE_ALIVE_COLORS: list[Tuple[int, int, int]] = [
    (100, 160, 255),  # Node 0: Bright Blue
    (80, 200, 120),  # Node 1: Bright Green
    (255, 210, 80),  # Node 2: Bright Yellow
    (255, 100, 100),  # Node 3: Bright Red
]

# Dead cell colors (dimmed versions of node colors)
NODE_DEAD_COLORS: list[Tuple[int, int, int]] = [
    (20, 40, 80),  # Node 0: Dark Blue
    (15, 50, 30),  # Node 1: Dark Green
    (60, 50, 20),  # Node 2: Dark Yellow
    (60, 25, 25),  # Node 3: Dark Red
]

# UI Colors
BACKGROUND_COLOR: Tuple[int, int, int] = (20, 20, 20)
STATS_PANEL_BG: Tuple[int, int, int] = (30, 30, 40)
BOUNDARY_COLOR: Tuple[int, int, int] = (255, 100, 100)
FAULT_FLASH_COLOR: Tuple[int, int, int] = (255, 255, 0)
TEXT_COLOR: Tuple[int, int, int] = (220, 220, 220)
TEXT_HIGHLIGHT_COLOR: Tuple[int, int, int] = (255, 255, 255)

# ==============================================================================
# Layout Constants
# ==============================================================================

STATS_PANEL_WIDTH: int = 280
DEFAULT_CELL_SIZE: int = 10
MIN_CELL_SIZE: int = 4
MAX_CELL_SIZE: int = 20

# ==============================================================================
# Animation Constants
# ==============================================================================

FAULT_FLASH_FRAMES: int = 10  # Number of frames for fault flash animation
DEFAULT_FPS: int = 10  # Target frames per second


# ==============================================================================
# Configuration Dataclass
# ==============================================================================


@dataclass
class VisualizerConfig:
    """Configuration for the DSM visualizer."""

    # Grid dimensions
    grid_width: int = 60
    grid_height: int = 60

    # DSM configuration
    num_nodes: int = 2

    # Display settings
    cell_size: int = DEFAULT_CELL_SIZE
    fps: int = DEFAULT_FPS

    # Mode flags
    demo_mode: bool = True
    show_stats: bool = True

    # Paths (for live/replay modes)
    gol_executable: str = ""
    stats_dir: str = ""

    @property
    def grid_pixel_width(self) -> int:
        """Width of the grid area in pixels."""
        return self.grid_width * self.cell_size

    @property
    def grid_pixel_height(self) -> int:
        """Height of the grid area in pixels."""
        return self.grid_height * self.cell_size

    @property
    def window_width(self) -> int:
        """Total window width including stats panel."""
        if self.show_stats:
            return self.grid_pixel_width + STATS_PANEL_WIDTH
        return self.grid_pixel_width

    @property
    def window_height(self) -> int:
        """Total window height."""
        # Calculate minimum height based on number of nodes
        # Each node needs ~160px, plus header(80) + totals(120) + controls(130)
        min_for_stats = 80 + (self.num_nodes * 160) + 120 + 130
        return max(self.grid_pixel_height, min_for_stats)


@dataclass
class DSMStatsSnapshot:
    """Snapshot of DSM statistics at a point in time."""

    page_faults: int = 0
    read_faults: int = 0
    write_faults: int = 0
    pages_fetched: int = 0
    pages_sent: int = 0
    invalidations_sent: int = 0
    invalidations_received: int = 0
    network_bytes_sent: int = 0
    network_bytes_received: int = 0
    lock_acquires: int = 0
    barrier_waits: int = 0

    # Performance metrics
    total_fault_latency_ns: int = 0
    max_fault_latency_ns: int = 0
    min_fault_latency_ns: int = 0

    @property
    def avg_fault_latency_ms(self) -> float:
        """Average fault latency in milliseconds."""
        if self.page_faults == 0:
            return 0.0
        return (self.total_fault_latency_ns / self.page_faults) / 1_000_000

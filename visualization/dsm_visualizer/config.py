"""Configuration and constants for the DSM visualizer."""

from dataclasses import dataclass, field
from typing import Tuple

# ==============================================================================
# Color Scheme (Modern, Polished)
# ==============================================================================

# Node colors - each node gets a distinct color for its partition
NODE_COLORS: list[Tuple[int, int, int]] = [
    (79, 172, 254),  # Node 0: Sky Blue
    (67, 217, 173),  # Node 1: Mint Green
    (255, 209, 102),  # Node 2: Golden Yellow
    (255, 107, 129),  # Node 3: Coral Red
]

# Alive cell colors (vibrant versions of node colors)
NODE_ALIVE_COLORS: list[Tuple[int, int, int]] = [
    (99, 179, 237),  # Node 0: Bright Sky Blue
    (72, 207, 173),  # Node 1: Bright Mint
    (255, 215, 94),  # Node 2: Bright Gold
    (255, 118, 117),  # Node 3: Bright Coral
]

# Dead cell colors (rich dark versions of node colors)
NODE_DEAD_COLORS: list[Tuple[int, int, int]] = [
    (26, 54, 93),  # Node 0: Deep Blue
    (22, 66, 60),  # Node 1: Deep Teal
    (77, 61, 28),  # Node 2: Deep Amber
    (87, 35, 45),  # Node 3: Deep Rose
]

# Accent colors for UI elements (matching node themes)
NODE_ACCENT_COLORS: list[Tuple[int, int, int]] = [
    (59, 130, 246),  # Node 0: Blue accent
    (16, 185, 129),  # Node 1: Green accent
    (245, 158, 11),  # Node 2: Amber accent
    (239, 68, 68),  # Node 3: Red accent
]

# UI Colors
BACKGROUND_COLOR: Tuple[int, int, int] = (15, 15, 20)
STATS_PANEL_BG: Tuple[int, int, int] = (22, 24, 32)
STATS_PANEL_CARD_BG: Tuple[int, int, int] = (32, 34, 45)
STATS_PANEL_HEADER_BG: Tuple[int, int, int] = (38, 40, 55)
BOUNDARY_COLOR: Tuple[int, int, int] = (180, 80, 80)
FAULT_FLASH_COLOR: Tuple[int, int, int] = (255, 235, 59)
TEXT_COLOR: Tuple[int, int, int] = (200, 205, 215)
TEXT_DIM_COLOR: Tuple[int, int, int] = (140, 145, 160)
TEXT_HIGHLIGHT_COLOR: Tuple[int, int, int] = (255, 255, 255)
ACCENT_COLOR: Tuple[int, int, int] = (99, 179, 237)
SUCCESS_COLOR: Tuple[int, int, int] = (72, 207, 173)
WARNING_COLOR: Tuple[int, int, int] = (255, 215, 94)
SEPARATOR_COLOR: Tuple[int, int, int] = (50, 55, 70)

# ==============================================================================
# Layout Constants
# ==============================================================================

STATS_PANEL_WIDTH: int = 320
DEFAULT_CELL_SIZE: int = 12  # Slightly larger for better visibility
MIN_CELL_SIZE: int = 4
MAX_CELL_SIZE: int = 20
CELL_BORDER_RADIUS: int = 2
CARD_BORDER_RADIUS: int = 8
PANEL_PADDING: int = 16
CARD_PADDING: int = 12

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
        """Total window height - matches grid height exactly."""
        return self.grid_pixel_height


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

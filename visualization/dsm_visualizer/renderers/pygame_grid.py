"""Pygame-based grid renderer for Game of Life visualization."""

import pygame
from dataclasses import dataclass
from typing import Dict, Optional, Set, Tuple

from dsm_visualizer.config import (
    VisualizerConfig,
    NODE_ALIVE_COLORS,
    NODE_DEAD_COLORS,
    BACKGROUND_COLOR,
    BOUNDARY_COLOR,
    FAULT_FLASH_COLOR,
    FAULT_FLASH_FRAMES,
)
from dsm_visualizer.models.grid_state import GridState
from dsm_visualizer.models.dsm_stats import DSMStats
from dsm_visualizer.renderers.stats_panel import StatsPanel


@dataclass
class RenderResult:
    """Result of a render call with user input information."""

    should_quit: bool = False
    toggle_pause: bool = False
    step_once: bool = False
    reset: bool = False
    speed_up: bool = False
    speed_down: bool = False


class PygameGridRenderer:
    """
    Renders the Game of Life grid with DSM partition visualization.

    Features:
    - Cells colored by partition owner
    - Partition boundary lines
    - Page fault flash animations
    - Stats panel sidebar
    """

    def __init__(self, config: VisualizerConfig):
        """
        Initialize the pygame renderer.

        Args:
            config: Visualizer configuration.
        """
        self.config = config
        self.cell_size = config.cell_size

        # Initialize pygame
        pygame.init()
        pygame.display.set_caption("DSM Game of Life Visualizer")

        # Create display
        self.screen = pygame.display.set_mode(
            (config.window_width, config.window_height)
        )

        # Create stats panel
        if config.show_stats:
            self.stats_panel = StatsPanel(
                self.screen,
                x_offset=config.grid_pixel_width,
                width=280,
                height=config.window_height,
            )
        else:
            self.stats_panel = None

        # Fault animation state: {(row, col): frames_remaining}
        self.active_faults: Dict[Tuple[int, int], int] = {}

        # Pause state for display
        self.paused = False

        # Pre-render surfaces for efficiency (optional optimization)
        self.clock = pygame.time.Clock()

    def render(
        self,
        grid: GridState,
        stats: DSMStats,
        generation: int = 0,
        paused: bool = False,
        status: str = "",
    ) -> RenderResult:
        """
        Render the complete visualization frame.

        Args:
            grid: Current grid state.
            stats: Current DSM statistics.
            generation: Current generation number.
            paused: Whether simulation is paused.
            status: Optional status text (e.g., "RUNNING", "WAITING").

        Returns:
            RenderResult with user input flags.
        """
        result = RenderResult()
        self.paused = paused

        # Handle events
        for event in pygame.event.get():
            if event.type == pygame.QUIT:
                result.should_quit = True
            elif event.type == pygame.KEYDOWN:
                if event.key == pygame.K_ESCAPE or event.key == pygame.K_q:
                    result.should_quit = True
                elif event.key == pygame.K_SPACE:
                    result.toggle_pause = True
                elif event.key == pygame.K_n or event.key == pygame.K_RIGHT:
                    result.step_once = True
                elif event.key == pygame.K_r:
                    result.reset = True
                elif event.key == pygame.K_UP or event.key == pygame.K_EQUALS:
                    result.speed_up = True
                elif event.key == pygame.K_DOWN or event.key == pygame.K_MINUS:
                    result.speed_down = True

        # Clear screen
        self.screen.fill(BACKGROUND_COLOR)

        # Draw grid cells
        self._draw_cells(grid)

        # Draw partition boundaries
        self._draw_partition_boundaries(grid)

        # Draw fault animations
        self._draw_fault_highlights(grid)

        # Update fault animation state
        self._update_fault_animations()

        # Draw stats panel
        if self.stats_panel:
            self.stats_panel.render(stats, generation, grid, paused, status)

        # Draw pause overlay if paused
        if paused:
            self._draw_pause_overlay(grid)

        # Update display
        pygame.display.flip()

        # Cap framerate
        self.clock.tick(self.config.fps)

        return result

    def _draw_pause_overlay(self, grid: GridState) -> None:
        """Draw a semi-transparent pause indicator."""
        # Create overlay surface
        overlay = pygame.Surface(
            (self.config.grid_pixel_width, self.config.grid_pixel_height),
            pygame.SRCALPHA,
        )
        overlay.fill((0, 0, 0, 100))  # Semi-transparent black
        self.screen.blit(overlay, (0, 0))

        # Draw "PAUSED" text
        font = pygame.font.SysFont("monospace", 48, bold=True)
        text = font.render("PAUSED", True, (255, 255, 255))
        text_rect = text.get_rect(
            center=(
                self.config.grid_pixel_width // 2,
                self.config.grid_pixel_height // 2,
            )
        )
        self.screen.blit(text, text_rect)

        # Draw hint text
        hint_font = pygame.font.SysFont("monospace", 16)
        hint = hint_font.render(
            "Press SPACE to resume, N to step", True, (200, 200, 200)
        )
        hint_rect = hint.get_rect(
            center=(
                self.config.grid_pixel_width // 2,
                self.config.grid_pixel_height // 2 + 40,
            )
        )
        self.screen.blit(hint, hint_rect)

    def _draw_cells(self, grid: GridState) -> None:
        """Draw all cells with partition-based coloring."""
        # First, fill any extra space below grid with last partition's dead color
        grid_pixel_height = grid.height * self.cell_size
        if self.config.window_height > grid_pixel_height:
            last_owner = grid.num_nodes - 1
            fill_color = NODE_DEAD_COLORS[last_owner % len(NODE_DEAD_COLORS)]
            pygame.draw.rect(
                self.screen,
                fill_color,
                (
                    0,
                    grid_pixel_height,
                    self.config.grid_pixel_width,
                    self.config.window_height - grid_pixel_height,
                ),
            )

        # Draw all cells
        for row in range(grid.height):
            owner = grid.get_owner(row)
            alive_color = NODE_ALIVE_COLORS[owner % len(NODE_ALIVE_COLORS)]
            dead_color = NODE_DEAD_COLORS[owner % len(NODE_DEAD_COLORS)]

            for col in range(grid.width):
                x = col * self.cell_size
                y = row * self.cell_size

                # Choose color based on cell state
                if grid.cells[row, col] == 1:
                    color = alive_color
                else:
                    color = dead_color

                # Draw cell (with 1px gap for grid effect)
                pygame.draw.rect(
                    self.screen,
                    color,
                    (x, y, self.cell_size - 1, self.cell_size - 1),
                )

    def _draw_partition_boundaries(self, grid: GridState) -> None:
        """Draw lines at partition boundaries."""
        for i, (start, end) in enumerate(grid.partition_boundaries):
            if i > 0:  # Don't draw at top of first partition
                y = start * self.cell_size
                pygame.draw.line(
                    self.screen,
                    BOUNDARY_COLOR,
                    (0, y),
                    (grid.width * self.cell_size, y),
                    3,  # Line thickness
                )

    def _draw_fault_highlights(self, grid: GridState) -> None:
        """Draw yellow highlights for active page faults."""
        for (row, col), frames in self.active_faults.items():
            if frames > 0:
                x = col * self.cell_size
                y = row * self.cell_size

                # Calculate alpha based on remaining frames
                alpha = int(255 * (frames / FAULT_FLASH_FRAMES))

                # Create a surface with alpha for the highlight
                highlight = pygame.Surface(
                    (self.cell_size - 1, self.cell_size - 1),
                    pygame.SRCALPHA,
                )
                highlight.fill((*FAULT_FLASH_COLOR, alpha))
                self.screen.blit(highlight, (x, y))

    def _update_fault_animations(self) -> None:
        """Decrement fault animation counters and remove finished ones."""
        to_remove = []
        for key in self.active_faults:
            self.active_faults[key] -= 1
            if self.active_faults[key] <= 0:
                to_remove.append(key)

        for key in to_remove:
            del self.active_faults[key]

    def trigger_fault_at_row(self, row: int, grid_width: int) -> None:
        """
        Trigger a page fault animation for an entire row.

        Args:
            row: The row where the fault occurred.
            grid_width: Width of the grid.
        """
        for col in range(grid_width):
            self.active_faults[(row, col)] = FAULT_FLASH_FRAMES

    def trigger_fault_at_cell(self, row: int, col: int) -> None:
        """
        Trigger a page fault animation for a specific cell.

        Args:
            row: Row of the fault.
            col: Column of the fault.
        """
        self.active_faults[(row, col)] = FAULT_FLASH_FRAMES

    def cleanup(self) -> None:
        """Clean up pygame resources."""
        pygame.quit()

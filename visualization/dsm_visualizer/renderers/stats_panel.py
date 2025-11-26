"""Stats panel renderer for DSM statistics display."""

import pygame
from typing import Optional

from dsm_visualizer.config import (
    STATS_PANEL_BG,
    TEXT_COLOR,
    TEXT_HIGHLIGHT_COLOR,
    NODE_ALIVE_COLORS,
)
from dsm_visualizer.models.grid_state import GridState
from dsm_visualizer.models.dsm_stats import DSMStats


class StatsPanel:
    """
    Renders the statistics sidebar panel.

    Displays:
    - Current generation
    - Per-node statistics (faults, network traffic)
    - Totals
    """

    def __init__(
        self,
        screen: pygame.Surface,
        x_offset: int,
        width: int,
        height: int,
    ):
        """
        Initialize the stats panel.

        Args:
            screen: Pygame surface to draw on.
            x_offset: X position where panel starts.
            width: Width of the panel.
            height: Height of the panel.
        """
        self.screen = screen
        self.x = x_offset
        self.width = width
        self.height = height

        # Initialize fonts
        pygame.font.init()
        self.title_font = pygame.font.SysFont("monospace", 16, bold=True)
        self.header_font = pygame.font.SysFont("monospace", 14, bold=True)
        self.font = pygame.font.SysFont("monospace", 12)

        # Layout constants
        self.padding = 10
        self.line_height = 18
        self.section_gap = 10

    def render(
        self,
        stats: DSMStats,
        generation: int,
        grid: Optional[GridState] = None,
        paused: bool = False,
        status: str = "",
    ) -> None:
        """
        Render the stats panel.

        Args:
            stats: Current DSM statistics.
            generation: Current generation number.
            grid: Optional grid state for live cell counts.
            paused: Whether simulation is paused.
            status: Optional status text (e.g., "RUNNING", "WAITING").
        """
        # Draw panel background
        pygame.draw.rect(
            self.screen,
            STATS_PANEL_BG,
            (self.x, 0, self.width, self.height),
        )

        # Draw border
        pygame.draw.line(
            self.screen,
            (60, 60, 70),
            (self.x, 0),
            (self.x, self.height),
            2,
        )

        y = self.padding

        # Title
        y = self._draw_text(
            "═══ DSM Statistics ═══",
            y,
            self.title_font,
            TEXT_HIGHLIGHT_COLOR,
            center=True,
        )
        y += self.section_gap

        # Status indicator (for live mode)
        if status:
            status_color = (0, 255, 100) if status == "RUNNING" else (255, 200, 0)
            y = self._draw_text(f"Status: {status}", y, self.header_font, status_color)
            y += self.section_gap // 2

        # Generation counter
        y = self._draw_text(f"Generation: {generation}", y, self.header_font)
        y += self.section_gap // 2

        # Live cells count
        if grid:
            total_live = grid.count_live_cells()
            y = self._draw_text(f"Live Cells: {total_live}", y)
        y += self.section_gap

        # Separator
        y = self._draw_separator(y)
        y += self.section_gap // 2

        # Per-node statistics
        for node_id in sorted(stats.node_stats.keys()):
            node_stats = stats.node_stats[node_id]
            color = NODE_ALIVE_COLORS[node_id % len(NODE_ALIVE_COLORS)]

            # Node header
            y = self._draw_text(f"─── Node {node_id} ───", y, self.header_font, color)

            # Partition info
            if grid:
                start, end = grid.partition_boundaries[node_id]
                y = self._draw_text(f"  Rows: {start}-{end-1}", y)
                live_in_partition = grid.count_live_cells_in_partition(node_id)
                y = self._draw_text(f"  Live: {live_in_partition}", y)

            # Fault counts
            y = self._draw_text(f"  Faults: {node_stats.page_faults}", y)
            y = self._draw_text(f"    Read:  {node_stats.read_faults}", y)
            y = self._draw_text(f"    Write: {node_stats.write_faults}", y)

            # Network traffic
            y = self._draw_text(f"  Net TX: {node_stats.bytes_sent_kb:.1f} KB", y)
            y = self._draw_text(f"  Net RX: {node_stats.bytes_received_kb:.1f} KB", y)

            y += self.section_gap // 2

        # Separator
        y = self._draw_separator(y)
        y += self.section_gap // 2

        # Totals
        y = self._draw_text("─── TOTALS ───", y, self.header_font, TEXT_HIGHLIGHT_COLOR)
        y = self._draw_text(f"  Total Faults: {stats.total_page_faults}", y)
        y = self._draw_text(f"    Read:  {stats.total_read_faults}", y)
        y = self._draw_text(f"    Write: {stats.total_write_faults}", y)
        y = self._draw_text(f"  Barriers: {stats.total_barriers}", y)

        total_kb = (stats.total_bytes_sent + stats.total_bytes_received) / 1024
        y = self._draw_text(f"  Network: {total_kb:.1f} KB", y)

        y += self.section_gap

        # Separator before controls
        y = self._draw_separator(y)
        y += self.section_gap // 2

        # Controls section (flows after content, not fixed position)
        y = self._draw_text(
            "─── Controls ───", y, self.header_font, TEXT_HIGHLIGHT_COLOR
        )
        y = self._draw_text("  SPACE: Pause/Resume", y)
        y = self._draw_text("  N/→: Step once", y)
        y = self._draw_text("  R: Reset", y)
        y = self._draw_text("  ↑/↓: Speed +/-", y)
        y = self._draw_text("  Q/ESC: Quit", y)

    def _draw_text(
        self,
        text: str,
        y: int,
        font: Optional[pygame.font.Font] = None,
        color: tuple = TEXT_COLOR,
        center: bool = False,
    ) -> int:
        """
        Draw text at the specified position.

        Args:
            text: Text to draw.
            y: Y position.
            font: Font to use (defaults to regular font).
            color: Text color.
            center: Whether to center the text.

        Returns:
            Y position after this text (for chaining).
        """
        if font is None:
            font = self.font

        surface = font.render(text, True, color)

        if center:
            x = self.x + (self.width - surface.get_width()) // 2
        else:
            x = self.x + self.padding

        self.screen.blit(surface, (x, y))
        return y + self.line_height

    def _draw_separator(self, y: int) -> int:
        """Draw a horizontal separator line."""
        pygame.draw.line(
            self.screen,
            (60, 60, 70),
            (self.x + self.padding, y),
            (self.x + self.width - self.padding, y),
            1,
        )
        return y + 5

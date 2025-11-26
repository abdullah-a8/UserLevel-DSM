"""Stats panel renderer for DSM statistics display - Modern UI design."""

import pygame
from typing import Optional, Tuple

from dsm_visualizer.config import (
    STATS_PANEL_BG,
    STATS_PANEL_CARD_BG,
    STATS_PANEL_HEADER_BG,
    TEXT_COLOR,
    TEXT_DIM_COLOR,
    TEXT_HIGHLIGHT_COLOR,
    NODE_ALIVE_COLORS,
    NODE_ACCENT_COLORS,
    SEPARATOR_COLOR,
    SUCCESS_COLOR,
    WARNING_COLOR,
    ACCENT_COLOR,
    CARD_BORDER_RADIUS,
    PANEL_PADDING,
    CARD_PADDING,
)
from dsm_visualizer.models.grid_state import GridState
from dsm_visualizer.models.dsm_stats import DSMStats


class StatsPanel:
    """
    Renders a modern, polished statistics sidebar panel.

    Features:
    - Card-based layout for each section
    - Better typography and spacing
    - Status indicators with icons
    - Scrollable content for many nodes
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
        self.title_font = pygame.font.SysFont("monospace", 18, bold=True)
        self.header_font = pygame.font.SysFont("monospace", 14, bold=True)
        self.label_font = pygame.font.SysFont("monospace", 11, bold=True)
        self.value_font = pygame.font.SysFont("monospace", 12)
        self.small_font = pygame.font.SysFont("monospace", 10)

        # Layout constants
        self.padding = PANEL_PADDING
        self.card_padding = CARD_PADDING
        self.line_height = 16
        self.card_gap = 10

        # Scrolling support
        self.scroll_offset = 0
        self.max_scroll = 0
        self.content_height = 0

        # Create a large surface for content (will be scrolled)
        self.content_surface = pygame.Surface((width, 2000), pygame.SRCALPHA)
        # Panel surface is the visible window
        self.panel_surface = pygame.Surface((width, height), pygame.SRCALPHA)

    def _draw_rounded_rect(
        self,
        surface: pygame.Surface,
        rect: Tuple[int, int, int, int],
        color: Tuple[int, int, int],
        radius: int = CARD_BORDER_RADIUS,
        border_color: Optional[Tuple[int, int, int]] = None,
        border_width: int = 1,
    ) -> None:
        """Draw a rounded rectangle with optional border."""
        x, y, w, h = rect

        # Draw filled rounded rect
        pygame.draw.rect(surface, color, (x, y, w, h), border_radius=radius)

        # Draw border if specified
        if border_color:
            pygame.draw.rect(
                surface,
                border_color,
                (x, y, w, h),
                width=border_width,
                border_radius=radius,
            )

    def _draw_status_indicator(self, y: int, status: str, paused: bool) -> int:
        """Draw a modern status indicator."""
        # Status card
        card_x = self.padding
        card_w = self.width - 2 * self.padding
        card_h = 32

        if status == "RUNNING" and not paused:
            bg_color = (22, 78, 55)  # Dark green
            text_color = SUCCESS_COLOR
            status_text = "[>] RUNNING"
        elif paused:
            bg_color = (78, 62, 22)  # Dark amber
            text_color = WARNING_COLOR
            status_text = "[||] PAUSED"
        elif status == "COMPLETE":
            bg_color = (22, 54, 78)  # Dark blue
            text_color = ACCENT_COLOR
            status_text = "[OK] COMPLETE"
        else:
            bg_color = (45, 45, 55)
            text_color = TEXT_DIM_COLOR
            status_text = "[..] " + status

        self._draw_rounded_rect(
            self.content_surface,
            (card_x, y, card_w, card_h),
            bg_color,
            radius=6,
        )

        # Draw status text centered
        text_surf = self.header_font.render(status_text, True, text_color)
        text_x = card_x + (card_w - text_surf.get_width()) // 2
        text_y = y + (card_h - text_surf.get_height()) // 2
        self.content_surface.blit(text_surf, (text_x, text_y))

        return y + card_h + 8

    def _draw_header_card(self, y: int, generation: int, live_cells: int) -> int:
        """Draw the main header card with generation and live cells."""
        card_x = self.padding
        card_w = self.width - 2 * self.padding
        card_h = 70

        # Header card background
        self._draw_rounded_rect(
            self.content_surface,
            (card_x, y, card_w, card_h),
            STATS_PANEL_HEADER_BG,
            border_color=SEPARATOR_COLOR,
        )

        # Title
        title_surf = self.title_font.render(
            "DSM Statistics", True, TEXT_HIGHLIGHT_COLOR
        )
        title_x = card_x + (card_w - title_surf.get_width()) // 2
        self.content_surface.blit(title_surf, (title_x, y + 8))

        # Stats row
        inner_y = y + 35
        col_width = (card_w - 2 * self.card_padding) // 2

        # Generation
        gen_label = self.small_font.render("GENERATION", True, TEXT_DIM_COLOR)
        self.content_surface.blit(gen_label, (card_x + self.card_padding, inner_y))
        gen_value = self.header_font.render(str(generation), True, ACCENT_COLOR)
        self.content_surface.blit(gen_value, (card_x + self.card_padding, inner_y + 12))

        # Live cells
        live_label = self.small_font.render("LIVE CELLS", True, TEXT_DIM_COLOR)
        self.content_surface.blit(
            live_label, (card_x + self.card_padding + col_width, inner_y)
        )
        live_value = self.header_font.render(str(live_cells), True, SUCCESS_COLOR)
        self.content_surface.blit(
            live_value, (card_x + self.card_padding + col_width, inner_y + 12)
        )

        return y + card_h + self.card_gap

    def _draw_node_card(
        self, y: int, node_id: int, node_stats, grid: Optional[GridState]
    ) -> int:
        """Draw a card for a single node's statistics."""
        card_x = self.padding
        card_w = self.width - 2 * self.padding
        card_h = 95  # Compact height

        node_color = NODE_ALIVE_COLORS[node_id % len(NODE_ALIVE_COLORS)]

        # Card background
        self._draw_rounded_rect(
            self.content_surface,
            (card_x, y, card_w, card_h),
            STATS_PANEL_CARD_BG,
        )

        # Color accent bar on left
        accent_rect = (card_x, y, 4, card_h)
        pygame.draw.rect(
            self.content_surface,
            node_color,
            accent_rect,
            border_top_left_radius=CARD_BORDER_RADIUS,
            border_bottom_left_radius=CARD_BORDER_RADIUS,
        )

        # Node header
        inner_x = card_x + self.card_padding + 4
        inner_y = y + 8

        node_label = self.header_font.render(f"Node {node_id}", True, node_color)
        self.content_surface.blit(node_label, (inner_x, inner_y))

        # Partition info (rows)
        if grid:
            start, end = grid.partition_boundaries[node_id]
            rows_text = f"Rows {start}-{end-1}"
            rows_surf = self.small_font.render(rows_text, True, TEXT_DIM_COLOR)
            self.content_surface.blit(rows_surf, (inner_x + 70, inner_y + 2))

            # Live cells in partition
            live_in_partition = grid.count_live_cells_in_partition(node_id)
            live_surf = self.small_font.render(
                f"[{live_in_partition} alive]", True, node_color
            )
            self.content_surface.blit(live_surf, (card_x + card_w - 80, inner_y + 2))

        # Stats grid (2 columns)
        inner_y += 24
        col_width = (card_w - 2 * self.card_padding - 8) // 2

        # Left column: Faults
        self._draw_stat_item(
            inner_x, inner_y, "Faults", str(node_stats.page_faults), TEXT_COLOR
        )
        self._draw_stat_item(
            inner_x, inner_y + 18, "  Read", str(node_stats.read_faults), TEXT_DIM_COLOR
        )
        self._draw_stat_item(
            inner_x,
            inner_y + 36,
            "  Write",
            str(node_stats.write_faults),
            TEXT_DIM_COLOR,
        )

        # Right column: Network
        right_x = inner_x + col_width
        self._draw_stat_item(
            right_x, inner_y, "Net TX", f"{node_stats.bytes_sent_kb:.1f} KB", TEXT_COLOR
        )
        self._draw_stat_item(
            right_x,
            inner_y + 18,
            "Net RX",
            f"{node_stats.bytes_received_kb:.1f} KB",
            TEXT_DIM_COLOR,
        )

        return y + card_h + 6

    def _draw_stat_item(
        self, x: int, y: int, label: str, value: str, color: Tuple[int, int, int]
    ) -> None:
        """Draw a single stat item (label: value)."""
        label_surf = self.small_font.render(label + ":", True, TEXT_DIM_COLOR)
        self.content_surface.blit(label_surf, (x, y))

        value_surf = self.value_font.render(value, True, color)
        self.content_surface.blit(value_surf, (x + 55, y))

    def _draw_totals_card(self, y: int, stats: DSMStats) -> int:
        """Draw the totals summary card."""
        card_x = self.padding
        card_w = self.width - 2 * self.padding
        card_h = 80

        # Card background with subtle gradient effect (darker at top)
        self._draw_rounded_rect(
            self.content_surface,
            (card_x, y, card_w, card_h),
            STATS_PANEL_HEADER_BG,
            border_color=ACCENT_COLOR,
        )

        # Title
        inner_x = card_x + self.card_padding
        inner_y = y + 8

        title_surf = self.label_font.render("TOTALS", True, ACCENT_COLOR)
        self.content_surface.blit(title_surf, (inner_x, inner_y))

        # Stats in 2 columns
        inner_y += 20
        col_width = (card_w - 2 * self.card_padding) // 2

        # Left column
        total_faults = stats.total_page_faults
        self._draw_stat_item(
            inner_x, inner_y, "Faults", str(total_faults), TEXT_HIGHLIGHT_COLOR
        )
        self._draw_stat_item(
            inner_x,
            inner_y + 16,
            "  Read",
            str(stats.total_read_faults),
            TEXT_DIM_COLOR,
        )
        self._draw_stat_item(
            inner_x,
            inner_y + 32,
            "  Write",
            str(stats.total_write_faults),
            TEXT_DIM_COLOR,
        )

        # Right column
        right_x = inner_x + col_width
        self._draw_stat_item(
            right_x, inner_y, "Barriers", str(stats.total_barriers), TEXT_COLOR
        )
        total_kb = (stats.total_bytes_sent + stats.total_bytes_received) / 1024
        self._draw_stat_item(
            right_x, inner_y + 16, "Network", f"{total_kb:.1f} KB", TEXT_COLOR
        )

        return y + card_h + self.card_gap

    def _draw_controls_card(self, y: int) -> int:
        """Draw the controls help card (legacy, checks height)."""
        return self._draw_controls_card_always(y)

    def _draw_controls_card_always(self, y: int) -> int:
        """Draw the controls help card (always draws)."""
        card_x = self.padding
        card_w = self.width - 2 * self.padding
        card_h = 90

        # Card background
        self._draw_rounded_rect(
            self.content_surface,
            (card_x, y, card_w, card_h),
            STATS_PANEL_CARD_BG,
        )

        inner_x = card_x + self.card_padding
        inner_y = y + 8

        # Title
        title_surf = self.label_font.render("CONTROLS", True, TEXT_DIM_COLOR)
        self.content_surface.blit(title_surf, (inner_x, inner_y))

        # Controls in 2 columns
        controls_left = [
            ("SPACE", "Pause"),
            ("N / ->", "Step"),
            ("R", "Reset"),
        ]
        controls_right = [
            ("+/-", "Speed"),
            ("Q", "Quit"),
        ]

        inner_y += 18
        for i, (key, action) in enumerate(controls_left):
            key_surf = self.small_font.render(key, True, ACCENT_COLOR)
            self.content_surface.blit(key_surf, (inner_x, inner_y + i * 15))
            action_surf = self.small_font.render(action, True, TEXT_DIM_COLOR)
            self.content_surface.blit(action_surf, (inner_x + 50, inner_y + i * 15))

        right_x = inner_x + (card_w - 2 * self.card_padding) // 2
        for i, (key, action) in enumerate(controls_right):
            key_surf = self.small_font.render(key, True, ACCENT_COLOR)
            self.content_surface.blit(key_surf, (right_x, inner_y + i * 15))
            action_surf = self.small_font.render(action, True, TEXT_DIM_COLOR)
            self.content_surface.blit(action_surf, (right_x + 35, inner_y + i * 15))

        return y + card_h

    def _draw_scroll_indicator(self) -> None:
        """Draw a scroll bar indicator on the right side of the panel."""
        if self.max_scroll <= 0:
            return

        bar_width = 4
        bar_x = self.width - bar_width - 4
        bar_height = max(30, int(self.height * (self.height / self.content_height)))

        # Calculate bar position based on scroll
        scroll_ratio = (
            self.scroll_offset / self.max_scroll if self.max_scroll > 0 else 0
        )
        bar_y = int(scroll_ratio * (self.height - bar_height))

        # Draw track
        pygame.draw.rect(
            self.panel_surface,
            (40, 42, 54),
            (bar_x, 0, bar_width, self.height),
            border_radius=2,
        )

        # Draw thumb
        pygame.draw.rect(
            self.panel_surface,
            (80, 85, 100),
            (bar_x, bar_y, bar_width, bar_height),
            border_radius=2,
        )

    def handle_scroll(self, event: pygame.event.Event) -> bool:
        """Handle mouse wheel scrolling. Returns True if event was handled."""
        if event.type == pygame.MOUSEWHEEL:
            # Check if mouse is over the panel
            mouse_x, mouse_y = pygame.mouse.get_pos()
            if mouse_x >= self.x:
                # Scroll speed
                scroll_amount = 30
                self.scroll_offset -= event.y * scroll_amount
                # Clamp scroll offset
                self.scroll_offset = max(0, min(self.scroll_offset, self.max_scroll))
                return True
        return False

    def render(
        self,
        stats: DSMStats,
        generation: int,
        grid: Optional[GridState] = None,
        paused: bool = False,
        status: str = "",
    ) -> None:
        """
        Render the stats panel with scrolling support.

        Args:
            stats: Current DSM statistics.
            generation: Current generation number.
            grid: Optional grid state for live cell counts.
            paused: Whether simulation is paused.
            status: Optional status text (e.g., "RUNNING", "WAITING").
        """
        # Clear content surface
        self.content_surface.fill(STATS_PANEL_BG)

        y = self.padding

        # Status indicator (if provided)
        if status:
            y = self._draw_status_indicator(y, status, paused)
        elif paused:
            y = self._draw_status_indicator(y, "PAUSED", paused)

        # Header card with generation and live cells
        total_live = grid.count_live_cells() if grid else 0
        y = self._draw_header_card(y, generation, total_live)

        # Node cards
        for node_id in sorted(stats.node_stats.keys()):
            node_stats = stats.node_stats[node_id]
            y = self._draw_node_card(y, node_id, node_stats, grid)

        # Totals card
        y = self._draw_totals_card(y, stats)

        # Controls card
        y = self._draw_controls_card_always(y)

        # Store content height and calculate max scroll
        self.content_height = y + self.padding
        self.max_scroll = max(0, self.content_height - self.height)

        # Clear panel surface and blit scrolled content
        self.panel_surface.fill(STATS_PANEL_BG)
        self.panel_surface.blit(self.content_surface, (0, -self.scroll_offset))

        # Draw left border accent (on top of content)
        pygame.draw.line(
            self.panel_surface,
            SEPARATOR_COLOR,
            (0, 0),
            (0, self.height),
            2,
        )

        # Draw scroll indicator if content is scrollable
        if self.max_scroll > 0:
            self._draw_scroll_indicator()

        # Blit panel surface to screen
        self.screen.blit(self.panel_surface, (self.x, 0))

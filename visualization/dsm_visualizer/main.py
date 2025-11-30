"""Main entry point for DSM Game of Life Visualizer."""

import argparse
import sys
import time
from pathlib import Path

from dsm_visualizer.config import VisualizerConfig
from dsm_visualizer.models.grid_state import GridState
from dsm_visualizer.models.dsm_stats import DSMStats, NodeStats
from dsm_visualizer.renderers.pygame_grid import PygameGridRenderer
from dsm_visualizer.simulation.demo_simulator import DemoSimulator
from dsm_visualizer.data_sources.csv_reader import CSVStatsReader, PerfLogReader
from dsm_visualizer.data_sources.process_monitor import (
    GameOfLifeMonitor,
    ProcessEvent,
    EventType,
)


def parse_args() -> argparse.Namespace:
    """Parse command line arguments."""
    parser = argparse.ArgumentParser(
        description="DSM Game of Life Visualizer",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Modes:
  demo    - Run standalone simulation with simulated DSM behavior (default)
  live    - Launch actual GoL processes and visualize in real-time
  replay  - Read CSV stats files and replay visualization

Examples:
  # Demo mode with default settings
  python -m dsm_visualizer

  # Demo mode with custom grid
  python -m dsm_visualizer --mode demo --grid 80x60 --nodes 4

  # Live mode - launches actual GoL binary
  python -m dsm_visualizer --mode live --gol-binary ../game_of_life/build/game_of_life --nodes 2

  # Replay mode - replay from CSV stats
  python -m dsm_visualizer --mode replay --stats-dir ./stats --nodes 2
        """,
    )

    # Mode selection
    parser.add_argument(
        "--mode",
        "-m",
        choices=["demo", "live", "replay"],
        default="demo",
        help="Visualization mode: demo, live, or replay (default: demo)",
    )

    parser.add_argument(
        "--grid",
        type=str,
        default="60x60",
        help="Grid dimensions as WIDTHxHEIGHT (e.g., 60x60)",
    )
    parser.add_argument(
        "--nodes",
        type=int,
        default=2,
        help="Number of DSM nodes",
    )
    parser.add_argument(
        "--cell-size",
        type=int,
        default=10,
        help="Size of each cell in pixels",
    )
    parser.add_argument(
        "--fps",
        type=int,
        default=10,
        help="Target frames per second",
    )
    parser.add_argument(
        "--no-stats",
        action="store_true",
        help="Hide the stats panel",
    )

    # Demo mode options
    parser.add_argument(
        "--pattern",
        type=str,
        default="random",
        choices=["random", "glider", "glider_gun", "acorn", "rpentomino"],
        help="Initial pattern for the grid (demo mode)",
    )
    parser.add_argument(
        "--density",
        type=float,
        default=0.3,
        help="Cell density for random pattern (0.0-1.0)",
    )

    # Live mode options
    parser.add_argument(
        "--gol-binary",
        type=str,
        default="../game_of_life/build/game_of_life",
        help="Path to Game of Life binary (live mode)",
    )
    parser.add_argument(
        "--generations",
        type=int,
        default=100,
        help="Number of generations to run (live mode)",
    )

    # Replay mode options
    parser.add_argument(
        "--stats-dir",
        type=str,
        default="./stats",
        help="Directory containing CSV stats files (replay mode)",
    )
    parser.add_argument(
        "--replay-speed",
        type=float,
        default=1.0,
        help="Replay speed multiplier (replay mode)",
    )

    return parser.parse_args()


def create_config_from_args(args: argparse.Namespace) -> VisualizerConfig:
    """Create a VisualizerConfig from parsed arguments."""
    # Parse grid dimensions
    try:
        width, height = map(int, args.grid.lower().split("x"))
    except ValueError:
        print(
            f"Error: Invalid grid format '{args.grid}'. Use WIDTHxHEIGHT (e.g., 60x60)"
        )
        sys.exit(1)

    return VisualizerConfig(
        grid_width=width,
        grid_height=height,
        num_nodes=args.nodes,
        cell_size=args.cell_size,
        fps=args.fps,
        demo_mode=(args.mode == "demo"),
        show_stats=not args.no_stats,
    )


def run_demo(config: VisualizerConfig, pattern: str, density: float) -> None:
    """
    Run the visualization in demo mode with animated Game of Life.

    Args:
        config: Visualizer configuration.
        pattern: Initial pattern name.
        density: Cell density for random pattern.
    """
    print("=" * 60)
    print("DSM Game of Life Visualizer - Demo Mode")
    print("=" * 60)
    print(f"Grid: {config.grid_width}x{config.grid_height}")
    print(f"Nodes: {config.num_nodes}")
    print(f"Pattern: {pattern}")
    print(f"FPS: {config.fps}")
    print("=" * 60)
    print("Controls:")
    print("  SPACE     - Pause/Resume")
    print("  N / →     - Step once (when paused)")
    print("  R         - Reset simulation")
    print("  ↑ / ↓     - Speed up/down")
    print("  Q / ESC   - Quit")
    print("=" * 60)

    # Create renderer
    renderer = PygameGridRenderer(config)

    # Create simulator
    simulator = DemoSimulator(config)

    # Set up fault animation callback
    def on_fault(row: int, owner: int) -> None:
        """Trigger fault animation when page fault occurs."""
        renderer.trigger_fault_at_row(row, config.grid_width)

    simulator.set_fault_callback(on_fault)

    # Initialize pattern
    if pattern == "random":
        simulator.initialize_random(density)
    else:
        simulator.initialize_pattern(pattern)

    # Main loop state
    running = True
    paused = False
    current_fps = config.fps

    try:
        while running:
            # Render current state
            result = renderer.render(
                simulator.get_grid(),
                simulator.get_stats(),
                simulator.get_generation(),
                paused,
            )

            # Handle user input
            if result.should_quit:
                running = False
            elif result.toggle_pause:
                paused = not paused
                print(f"{'Paused' if paused else 'Resumed'}")
            elif result.step_once and paused:
                simulator.step()
            elif result.reset:
                if pattern == "random":
                    simulator.initialize_random(density)
                else:
                    simulator.initialize_pattern(pattern)
                print("Reset simulation")
            elif result.speed_up:
                current_fps = min(60, current_fps + 2)
                renderer.config.fps = current_fps
                print(f"Speed: {current_fps} FPS")
            elif result.speed_down:
                current_fps = max(1, current_fps - 2)
                renderer.config.fps = current_fps
                print(f"Speed: {current_fps} FPS")

            # Step simulation if not paused
            if not paused:
                simulator.step()

    finally:
        renderer.cleanup()

    print(f"\nSimulation ended at generation {simulator.get_generation()}")
    print(f"Total page faults: {simulator.get_stats().total_page_faults}")


def run_live(config: VisualizerConfig, args: argparse.Namespace) -> None:
    """
    Run the visualization in live mode - launches actual GoL processes.

    This mode:
    1. Starts the manager process (node 0)
    2. Starts worker processes (nodes 1..n-1)
    3. Monitors their stdout for stats updates
    4. Visualizes the grid state and DSM statistics in real-time

    Args:
        config: Visualizer configuration.
        args: Parsed command line arguments.
    """
    print("=" * 60)
    print("DSM Game of Life Visualizer - Live Mode")
    print("=" * 60)
    print(f"Grid: {config.grid_width}x{config.grid_height}")
    print(f"Nodes: {config.num_nodes}")
    print(f"Binary: {args.gol_binary}")
    print(f"Generations: {args.generations}")
    print("=" * 60)
    print("Controls:")
    print("  Q / ESC   - Quit")
    print("=" * 60)

    # Verify binary exists
    binary_path = Path(args.gol_binary)
    if not binary_path.exists():
        print(f"Error: GoL binary not found at {binary_path.absolute()}")
        print("Please build the game_of_life first:")
        print("  cd game_of_life && make")
        sys.exit(1)

    # Create renderer
    renderer = PygameGridRenderer(config)

    # Create grid state
    grid = GridState(config.grid_width, config.grid_height, config.num_nodes)

    # Create DSM stats
    stats = DSMStats()
    for i in range(config.num_nodes):
        stats.set_node(i, NodeStats(node_id=i))

    # Create process monitor using the event-based GameOfLifeMonitor
    monitor = GameOfLifeMonitor(
        executable_path=str(binary_path.absolute()),
        num_nodes=config.num_nodes,
        grid_width=config.grid_width,
        grid_height=config.grid_height,
        generations=args.generations,
    )

    # Track stats per node for updates
    node_stats_cache: dict[int, NodeStats] = {
        i: NodeStats(node_id=i) for i in range(config.num_nodes)
    }
    current_generation = [0]  # Use list for mutability in closure
    last_generation = [-1]  # Track last generation to detect updates

    running = True
    status = "STARTING"

    try:
        # Start the GoL processes
        print("\nStarting GoL processes...")
        if not monitor.start():
            print("Failed to start GoL processes")
            renderer.cleanup()
            sys.exit(1)
        print("Processes started. Visualizing...\n")
        status = "RUNNING"

        # Initialize grid with random pattern to show something
        grid.randomize(density=0.3)

        while running:
            # Process events from the monitor
            events_processed = 0
            while True:
                event = monitor.get_event(timeout=0.005)
                if event is None:
                    break
                events_processed += 1

                # Handle different event types
                if event.event_type == EventType.GENERATION:
                    current_generation[0] = event.data["generation"]
                    stats.generation = event.data["generation"]

                    # Update grid simulation when generation changes
                    if current_generation[0] != last_generation[0]:
                        # Simulate one GoL step to show visual progress
                        from dsm_visualizer.simulation.gol_rules import GameOfLifeRules

                        new_grid = GameOfLifeRules.compute_next_generation(grid)
                        grid.cells = new_grid.cells
                        last_generation[0] = current_generation[0]

                elif event.event_type == EventType.PAGE_FAULTS:
                    node_id = event.node_id
                    ns = node_stats_cache[node_id]
                    ns.page_faults = event.data["total"]
                    ns.read_faults = event.data["read"]
                    ns.write_faults = event.data["write"]
                    stats.set_node(node_id, ns)

                    # Trigger fault animation at partition boundary
                    partition = monitor.partition_info.get(node_id)
                    if partition:
                        # Animate at the boundary row
                        boundary_row = (
                            partition[1] - 1
                            if node_id < config.num_nodes - 1
                            else partition[0]
                        )
                        renderer.trigger_fault_at_row(boundary_row, config.grid_width)

                elif event.event_type == EventType.NETWORK:
                    node_id = event.node_id
                    ns = node_stats_cache[node_id]
                    ns.bytes_sent = int(event.data["kb_sent"] * 1024)
                    ns.bytes_received = int(event.data["kb_received"] * 1024)
                    stats.set_node(node_id, ns)

                elif event.event_type == EventType.COMPLETE:
                    print(f"Node {event.node_id} completed")

            # Check if simulation is complete
            if not monitor.is_running():
                if status != "COMPLETE":
                    print("\nSimulation complete!")
                    print("Press SPACE to close the window")
                status = "COMPLETE"
                # Keep rendering and wait for user to press SPACE to close
                result = renderer.render(
                    grid, stats, current_generation[0], paused=False, status=status
                )
                if result.should_quit or result.toggle_pause:
                    running = False
                continue

            # Render current state with status
            result = renderer.render(
                grid, stats, current_generation[0], paused=False, status=status
            )

            if result.should_quit:
                running = False
                monitor.stop()

    except KeyboardInterrupt:
        print("\nInterrupted by user")
        monitor.stop()
    finally:
        renderer.cleanup()

    print(f"\nFinal generation: {current_generation[0]}")
    final_stats = stats.get_totals()
    print(f"Total page faults: {final_stats.page_faults}")
    print(
        f"Total bytes transferred: {final_stats.bytes_sent + final_stats.bytes_received}"
    )


def run_replay(config: VisualizerConfig, args: argparse.Namespace) -> None:
    """
    Run the visualization in replay mode - reads CSV stats files.

    This mode:
    1. Reads dsm_stats_nodeN.csv files from stats directory
    2. Optionally reads perf_log.csv for page fault events
    3. Displays the statistics with timing from the original run

    Args:
        config: Visualizer configuration.
        args: Parsed command line arguments.
    """
    print("=" * 60)
    print("DSM Game of Life Visualizer - Replay Mode")
    print("=" * 60)
    print(f"Grid: {config.grid_width}x{config.grid_height}")
    print(f"Nodes: {config.num_nodes}")
    print(f"Stats directory: {args.stats_dir}")
    print(f"Replay speed: {args.replay_speed}x")
    print("=" * 60)
    print("Controls:")
    print("  SPACE     - Pause/Resume")
    print("  ↑ / ↓     - Speed up/down")
    print("  Q / ESC   - Quit")
    print("=" * 60)

    stats_path = Path(args.stats_dir)
    if not stats_path.exists():
        print(f"Error: Stats directory not found at {stats_path.absolute()}")
        sys.exit(1)

    # Read stats files
    csv_reader = CSVStatsReader(str(stats_path))
    perf_reader = PerfLogReader(str(stats_path))

    # Load node stats
    all_node_stats = {}
    for i in range(config.num_nodes):
        node_stats = csv_reader.read_node_stats(i)
        if node_stats:
            all_node_stats[i] = node_stats
            print(f"  Loaded stats for node {i}")
        else:
            print(f"  Warning: No stats file found for node {i}")

    # Load perf events if available
    perf_events = perf_reader.read_events()
    if perf_events:
        print(f"  Loaded {len(perf_events)} performance events")

    if not all_node_stats:
        print("Error: No stats files found")
        sys.exit(1)

    # Create renderer
    renderer = PygameGridRenderer(config)

    # Create grid state (will be empty since we don't have grid data in replay)
    grid = GridState(config.grid_width, config.grid_height, config.num_nodes)

    # Create DSM stats from loaded data
    stats = DSMStats()
    for node_id, node_stats in all_node_stats.items():
        stats.set_node(node_id, node_stats)

    # Main display loop
    running = True
    paused = False
    replay_speed = args.replay_speed
    generation = 0
    event_index = 0

    try:
        while running:
            # Process perf events based on timing
            if perf_events and event_index < len(perf_events):
                event = perf_events[event_index]
                # For simplicity, process one event per frame
                if not paused:
                    if event.event_type == "PAGE_FAULT":
                        # Trigger fault animation
                        # Convert page_id to row (approximate)
                        row = event.page_id % config.grid_height
                        renderer.trigger_fault_at_row(row, config.grid_width)
                    event_index += 1
                    generation = event_index // max(1, len(perf_events) // 100)

            # Render current state
            result = renderer.render(grid, stats, generation, paused)

            if result.should_quit:
                running = False
            elif result.toggle_pause:
                paused = not paused
                print(f"{'Paused' if paused else 'Resumed'}")
            elif result.speed_up:
                replay_speed = min(10.0, replay_speed * 1.5)
                print(f"Replay speed: {replay_speed:.1f}x")
            elif result.speed_down:
                replay_speed = max(0.1, replay_speed / 1.5)
                print(f"Replay speed: {replay_speed:.1f}x")

            # Check if replay is complete
            if perf_events and event_index >= len(perf_events):
                print("\nReplay complete!")
                # Show final state for a moment
                for _ in range(30):
                    result = renderer.render(grid, stats, generation, paused=True)
                    if result.should_quit:
                        break
                running = False

    finally:
        renderer.cleanup()

    print("\nReplay finished")
    final_stats = stats.get_totals()
    print(f"Total page faults: {final_stats.page_faults}")


def main() -> None:
    """Main entry point."""
    args = parse_args()
    config = create_config_from_args(args)

    if args.mode == "demo":
        run_demo(config, args.pattern, args.density)
    elif args.mode == "live":
        run_live(config, args)
    elif args.mode == "replay":
        run_replay(config, args)
    else:
        print(f"Unknown mode: {args.mode}")
        sys.exit(1)


if __name__ == "__main__":
    main()

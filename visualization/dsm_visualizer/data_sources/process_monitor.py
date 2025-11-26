"""Process monitor for launching and monitoring Game of Life processes.

This module handles:
1. Launching the GoL manager and worker processes
2. Parsing their stdout output for status updates
3. Extracting generation and statistics information

Expected GoL output format (from gol_main.c):
    [Node 0] Generation 10: 423 live cells in partition
    [Node 0] Page faults: 124 (R: 100, W: 24)
    [Node 0] Network: 45.20 KB sent, 38.10 KB received
"""

import re
import subprocess
import threading
import queue
import os
import signal
from dataclasses import dataclass
from enum import Enum
from pathlib import Path
from typing import Callable, Dict, List, Optional, Tuple


class EventType(Enum):
    """Types of events parsed from GoL output."""

    GENERATION = "generation"
    PAGE_FAULTS = "page_faults"
    NETWORK = "network"
    BARRIER = "barrier"
    INIT = "init"
    COMPLETE = "complete"
    ERROR = "error"


@dataclass
class ProcessEvent:
    """Event parsed from GoL process output."""

    event_type: EventType
    node_id: int
    data: dict  # Event-specific data


# Regex patterns for parsing GoL output
# Based on actual output from gol_main.c:
# [Node 0] Generation 10: 423 live cells in partition
# [Node 0] Page faults: 124 (R: 100, W: 24)
# [Node 0] Network: 45.20 KB sent, 38.10 KB received
# [Node 0] Partition: rows [0, 50)
# [Node 0] Waiting at BARRIER_COMPUTE_0...
# [Node 0] Passed BARRIER_COMPUTE_0
# [Node 0] === Computation Complete ===
# [Node 0] Final live cells: 1234
PATTERNS = {
    # [Node 0] Generation 10: 423 live cells in partition
    "generation": re.compile(r"\[Node (\d+)\] Generation (\d+): (\d+) live cells"),
    # [Node 0] Page faults: 124 (R: 100, W: 24)
    "page_faults": re.compile(
        r"\[Node (\d+)\] Page faults: (\d+) \(R: (\d+), W: (\d+)\)"
    ),
    # [Node 0] Network: 45.20 KB sent, 38.10 KB received
    "network": re.compile(
        r"\[Node (\d+)\] Network: ([\d.]+) KB sent, ([\d.]+) KB received"
    ),
    # [Node 0] Waiting at BARRIER_COMPUTE_0...
    "barrier_wait": re.compile(r"\[Node (\d+)\] Waiting at (BARRIER_\w+)"),
    # [Node 0] Passed BARRIER_COMPUTE_0
    "barrier_pass": re.compile(r"\[Node (\d+)\] Passed (BARRIER_\w+)"),
    # [Node 0] DSM initialized successfully
    "dsm_init": re.compile(r"\[Node (\d+)\] DSM initialized"),
    # [Node 0] === Computation Complete ===
    "complete": re.compile(r"\[Node (\d+)\] === Computation Complete ==="),
    # [Node 0] Partition: rows [0, 50)
    "partition": re.compile(r"\[Node (\d+)\] Partition: rows \[(\d+), (\d+)\)"),
    # [Node 0] Final live cells: 1234
    "final_cells": re.compile(r"\[Node (\d+)\] Final live cells: (\d+)"),
}


def parse_line(line: str) -> Optional[ProcessEvent]:
    """
    Parse a line of GoL output into a ProcessEvent.

    Args:
        line: A line of stdout from the GoL process.

    Returns:
        ProcessEvent if the line matches a known pattern, None otherwise.
    """
    line = line.strip()
    if not line:
        return None

    # Try generation pattern
    match = PATTERNS["generation"].search(line)
    if match:
        return ProcessEvent(
            event_type=EventType.GENERATION,
            node_id=int(match.group(1)),
            data={
                "generation": int(match.group(2)),
                "live_cells": int(match.group(3)),
            },
        )

    # Try page faults pattern
    match = PATTERNS["page_faults"].search(line)
    if match:
        return ProcessEvent(
            event_type=EventType.PAGE_FAULTS,
            node_id=int(match.group(1)),
            data={
                "total": int(match.group(2)),
                "read": int(match.group(3)),
                "write": int(match.group(4)),
            },
        )

    # Try network pattern
    match = PATTERNS["network"].search(line)
    if match:
        return ProcessEvent(
            event_type=EventType.NETWORK,
            node_id=int(match.group(1)),
            data={
                "kb_sent": float(match.group(2)),
                "kb_received": float(match.group(3)),
            },
        )

    # Try barrier patterns
    match = PATTERNS["barrier_pass"].search(line)
    if match:
        return ProcessEvent(
            event_type=EventType.BARRIER,
            node_id=int(match.group(1)),
            data={"barrier": match.group(2), "action": "passed"},
        )

    # Try partition info
    match = PATTERNS["partition"].search(line)
    if match:
        return ProcessEvent(
            event_type=EventType.INIT,
            node_id=int(match.group(1)),
            data={
                "start_row": int(match.group(2)),
                "end_row": int(match.group(3)),
            },
        )

    # Try completion pattern
    match = PATTERNS["complete"].search(line)
    if match:
        return ProcessEvent(
            event_type=EventType.COMPLETE,
            node_id=int(match.group(1)),
            data={},
        )

    # Try final cells pattern
    match = PATTERNS["final_cells"].search(line)
    if match:
        return ProcessEvent(
            event_type=EventType.COMPLETE,
            node_id=int(match.group(1)),
            data={"final_live_cells": int(match.group(2))},
        )

    return None


class GameOfLifeMonitor:
    """
    Launches and monitors Game of Life DSM processes.

    This class:
    1. Launches the manager and worker processes
    2. Monitors their stdout in real-time
    3. Parses output into events
    4. Provides a queue of events for the visualizer
    """

    def __init__(
        self,
        executable_path: str,
        num_nodes: int = 2,
        grid_width: int = 100,
        grid_height: int = 100,
        generations: int = 100,
        pattern: str = "random",
        manager_host: str = "127.0.0.1",
        base_port: int = 5000,
    ):
        """
        Initialize the Game of Life monitor.

        Args:
            executable_path: Path to the game_of_life executable.
            num_nodes: Number of nodes (2-4).
            grid_width: Grid width.
            grid_height: Grid height.
            generations: Number of generations to run.
            pattern: Initial pattern (random, glider, rpentomino).
            manager_host: Manager host for workers to connect to.
            base_port: Base port number.
        """
        self.executable = Path(executable_path)
        self.num_nodes = num_nodes
        self.grid_width = grid_width
        self.grid_height = grid_height
        self.generations = generations
        self.pattern = pattern
        self.manager_host = manager_host
        self.base_port = base_port

        # Process management
        self.processes: Dict[int, subprocess.Popen] = {}
        self.output_threads: List[threading.Thread] = []
        self.event_queue: queue.Queue[ProcessEvent] = queue.Queue()
        self.running = False

        # State tracking
        self.current_generation: Dict[int, int] = {}
        self.partition_info: Dict[int, Tuple[int, int]] = {}

    def _build_manager_args(self) -> List[str]:
        """Build command-line arguments for the manager process."""
        return [
            str(self.executable),
            "--manager",
            "--nodes",
            str(self.num_nodes),
            "--grid",
            f"{self.grid_width}x{self.grid_height}",
            "--generations",
            str(self.generations),
            "--pattern",
            self.pattern,
            "--port",
            str(self.base_port),
            "--display-interval",
            "1",  # Report every generation for visualization
        ]

    def _build_worker_args(self, node_id: int) -> List[str]:
        """Build command-line arguments for a worker process."""
        return [
            str(self.executable),
            "--worker",
            "--node-id",
            str(node_id),
            "--nodes",
            str(self.num_nodes),
            "--manager-host",
            self.manager_host,
            "--grid",
            f"{self.grid_width}x{self.grid_height}",
            "--generations",
            str(self.generations),
            "--pattern",
            self.pattern,
            "--port",
            str(self.base_port),
            "--display-interval",
            "1",
        ]

    def _read_output(self, process: subprocess.Popen, node_id: int) -> None:
        """
        Read and parse output from a process (runs in a thread).

        Args:
            process: The subprocess to read from.
            node_id: The node ID for this process.
        """
        try:
            for line in iter(process.stdout.readline, ""):
                if not self.running:
                    break
                if line:
                    # Print for debugging
                    # print(f"[Raw Node {node_id}] {line.strip()}")

                    event = parse_line(line)
                    if event:
                        self.event_queue.put(event)

                        # Track generation
                        if event.event_type == EventType.GENERATION:
                            self.current_generation[node_id] = event.data["generation"]

                        # Track partition info
                        if (
                            event.event_type == EventType.INIT
                            and "start_row" in event.data
                        ):
                            self.partition_info[node_id] = (
                                event.data["start_row"],
                                event.data["end_row"],
                            )
        except Exception as e:
            self.event_queue.put(
                ProcessEvent(
                    event_type=EventType.ERROR,
                    node_id=node_id,
                    data={"error": str(e)},
                )
            )

    def start(self) -> bool:
        """
        Start the Game of Life processes.

        Returns:
            True if all processes started successfully.
        """
        if not self.executable.exists():
            print(f"Error: Executable not found: {self.executable}")
            return False

        self.running = True

        try:
            # Start manager (Node 0) first
            print(f"Starting manager (Node 0)...")
            manager_args = self._build_manager_args()
            print(f"  Command: {' '.join(manager_args)}")

            self.processes[0] = subprocess.Popen(
                manager_args,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                bufsize=1,  # Line buffered
            )

            # Start output reader thread for manager
            thread = threading.Thread(
                target=self._read_output,
                args=(self.processes[0], 0),
                daemon=True,
            )
            thread.start()
            self.output_threads.append(thread)

            # Give manager time to initialize
            import time

            time.sleep(1)

            # Start worker nodes
            for node_id in range(1, self.num_nodes):
                print(f"Starting worker (Node {node_id})...")
                worker_args = self._build_worker_args(node_id)
                print(f"  Command: {' '.join(worker_args)}")

                self.processes[node_id] = subprocess.Popen(
                    worker_args,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    text=True,
                    bufsize=1,
                )

                thread = threading.Thread(
                    target=self._read_output,
                    args=(self.processes[node_id], node_id),
                    daemon=True,
                )
                thread.start()
                self.output_threads.append(thread)

                # Small delay between workers
                time.sleep(0.5)

            print(f"All {self.num_nodes} processes started")
            return True

        except Exception as e:
            print(f"Error starting processes: {e}")
            self.stop()
            return False

    def stop(self) -> None:
        """Stop all Game of Life processes."""
        self.running = False

        for node_id, process in self.processes.items():
            if process.poll() is None:
                print(f"Terminating Node {node_id}...")
                try:
                    process.terminate()
                    process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    process.kill()

        self.processes.clear()
        self.output_threads.clear()

    def get_event(self, timeout: float = 0.1) -> Optional[ProcessEvent]:
        """
        Get the next event from the queue.

        Args:
            timeout: How long to wait for an event (seconds).

        Returns:
            ProcessEvent or None if no event available.
        """
        try:
            return self.event_queue.get(timeout=timeout)
        except queue.Empty:
            return None

    def is_running(self) -> bool:
        """Check if any processes are still running."""
        if not self.running:
            return False
        return any(p.poll() is None for p in self.processes.values())

    def get_current_generation(self) -> int:
        """Get the minimum generation across all nodes (for synchronization)."""
        if not self.current_generation:
            return 0
        return min(self.current_generation.values())

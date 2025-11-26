"""DSM statistics models."""

from dataclasses import dataclass, field
from typing import Dict


@dataclass
class NodeStats:
    """Statistics for a single DSM node."""

    node_id: int

    # Fault counters
    page_faults: int = 0
    read_faults: int = 0
    write_faults: int = 0

    # Page transfer counters
    pages_fetched: int = 0
    pages_sent: int = 0

    # Invalidation counters
    invalidations_sent: int = 0
    invalidations_received: int = 0

    # Network counters
    bytes_sent: int = 0
    bytes_received: int = 0

    # Synchronization counters
    lock_acquires: int = 0
    barrier_waits: int = 0

    # Performance metrics (in nanoseconds)
    total_fault_latency_ns: int = 0
    max_fault_latency_ns: int = 0
    min_fault_latency_ns: int = 0

    @property
    def avg_fault_latency_ms(self) -> float:
        """Average fault latency in milliseconds."""
        if self.page_faults == 0:
            return 0.0
        return (self.total_fault_latency_ns / self.page_faults) / 1_000_000

    @property
    def bytes_sent_kb(self) -> float:
        """Bytes sent in kilobytes."""
        return self.bytes_sent / 1024

    @property
    def bytes_received_kb(self) -> float:
        """Bytes received in kilobytes."""
        return self.bytes_received / 1024


@dataclass
class DSMStats:
    """Aggregated DSM statistics across all nodes."""

    node_stats: Dict[int, NodeStats] = field(default_factory=dict)
    generation: int = 0

    def get_node(self, node_id: int) -> NodeStats:
        """Get stats for a specific node, creating if needed."""
        if node_id not in self.node_stats:
            self.node_stats[node_id] = NodeStats(node_id=node_id)
        return self.node_stats[node_id]

    def set_node(self, node_id: int, stats: NodeStats) -> None:
        """Set stats for a specific node."""
        self.node_stats[node_id] = stats

    @property
    def total_page_faults(self) -> int:
        """Total page faults across all nodes."""
        return sum(ns.page_faults for ns in self.node_stats.values())

    @property
    def total_read_faults(self) -> int:
        """Total read faults across all nodes."""
        return sum(ns.read_faults for ns in self.node_stats.values())

    @property
    def total_write_faults(self) -> int:
        """Total write faults across all nodes."""
        return sum(ns.write_faults for ns in self.node_stats.values())

    @property
    def total_bytes_sent(self) -> int:
        """Total bytes sent across all nodes."""
        return sum(ns.bytes_sent for ns in self.node_stats.values())

    @property
    def total_bytes_received(self) -> int:
        """Total bytes received across all nodes."""
        return sum(ns.bytes_received for ns in self.node_stats.values())

    @property
    def total_barriers(self) -> int:
        """Total barrier waits across all nodes."""
        return sum(ns.barrier_waits for ns in self.node_stats.values())

    def get_totals(self) -> NodeStats:
        """Get aggregated stats as a NodeStats object."""
        totals = NodeStats(node_id=-1)  # -1 indicates totals
        for ns in self.node_stats.values():
            totals.page_faults += ns.page_faults
            totals.read_faults += ns.read_faults
            totals.write_faults += ns.write_faults
            totals.pages_fetched += ns.pages_fetched
            totals.pages_sent += ns.pages_sent
            totals.invalidations_sent += ns.invalidations_sent
            totals.invalidations_received += ns.invalidations_received
            totals.bytes_sent += ns.bytes_sent
            totals.bytes_received += ns.bytes_received
            totals.lock_acquires += ns.lock_acquires
            totals.barrier_waits += ns.barrier_waits
            totals.total_fault_latency_ns += ns.total_fault_latency_ns
            if ns.max_fault_latency_ns > totals.max_fault_latency_ns:
                totals.max_fault_latency_ns = ns.max_fault_latency_ns
            if totals.min_fault_latency_ns == 0 or (
                ns.min_fault_latency_ns > 0
                and ns.min_fault_latency_ns < totals.min_fault_latency_ns
            ):
                totals.min_fault_latency_ns = ns.min_fault_latency_ns
        return totals

    def reset(self) -> None:
        """Reset all statistics."""
        self.node_stats.clear()
        self.generation = 0

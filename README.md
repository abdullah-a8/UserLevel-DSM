# User-Level Distributed Shared Memory (DSM)

**Operating Systems Semester Project**

A user-space implementation of Distributed Shared Memory for multi-threaded parallel programs running across a cluster of machines.

## Overview

This project implements a DSM system entirely in user space (no kernel modifications) that provides the illusion of a single shared address space across multiple machines. When a thread accesses a page residing on another machine, a page fault is triggered and the DSM automatically fetches the page over the network.

## Key Features

- **User-Space Implementation**: Uses `mmap`, `mprotect`, and `SIGSEGV` signal handling
- **Page-Based Coherence**: 4KB pages with automatic migration
- **Consistency Protocol**: Single-Writer/Multiple-Reader invalidation-based protocol
- **Synchronization Primitives**: Distributed locks and barriers
- **Network Communication**: TCP sockets for reliable page transfers
- **Visual Demo**: Conway's Game of Life with real-time ownership visualization

## Architecture

### Core Components

1. **Memory Manager** (`src/memory/`)
   - Page allocation using `mmap`
   - Page table management
   - Permission control via `mprotect`

2. **Fault Handler** (`src/memory/`)
   - SIGSEGV signal handler for page faults
   - Read/Write fault differentiation
   - Page fetch coordination

3. **Network Layer** (`src/network/`)
   - TCP server/client infrastructure
   - Message serialization/deserialization
   - Request-response protocol

4. **Consistency Protocol** (`src/consistency/`)
   - Page ownership tracking
   - Invalidation protocol
   - Request queuing for concurrency

5. **Synchronization** (`src/sync/`)
   - Distributed locks
   - Barrier synchronization

## Building

```bash
# Build DSM library
make

# Build and run tests
make test

# Build demo applications
make demo

# Clean build artifacts
make clean
```

## Project Structure

```
UserLevel-DSM/
├── src/                    # Source code
│   ├── memory/            # Page management, fault handling
│   ├── network/           # Socket communication
│   ├── consistency/       # Coherence protocol
│   ├── sync/              # Locks and barriers
│   └── core/              # Initialization, logging
├── include/dsm/           # Public API headers
├── tests/                 # Unit and integration tests
├── demos/                 # Demo applications
│   └── game_of_life/     # Conway's Game of Life demo
├── docs/                  # Documentation
└── Makefile              # Build configuration
```

## Usage Example

```c
#include <dsm/dsm.h>

int main() {
    // Initialize DSM
    dsm_config_t config = {
        .node_id = 0,
        .port = 5000,
        .num_nodes = 4,
        .is_manager = true,
        .log_level = LOG_LEVEL_INFO
    };
    dsm_init(&config);

    // Allocate shared memory
    int *shared_array = dsm_malloc(1000 * sizeof(int));

    // Use shared memory (faults handled automatically)
    shared_array[0] = 42;

    // Synchronization
    dsm_lock_t *lock = dsm_lock_create(1);
    dsm_lock_acquire(lock);
    // Critical section
    dsm_lock_release(lock);

    // Barrier
    dsm_barrier(0, 4);  // Wait for all 4 nodes

    // Cleanup
    dsm_free(shared_array);
    dsm_finalize();
    return 0;
}
```

## Running the Demo

### Start Manager Node
```bash
./build/dsm_manager --port 5000 --nodes 4
```

### Start Worker Nodes
```bash
# On each machine
./build/dsm_worker --manager <manager-ip>:5000 --id <1-4>
```

### Run Game of Life Demo
```bash
./build/game_of_life --grid 1000 --nodes 4 --iterations 100
```

## Implementation Timeline

- **Days 1-2**: Project setup, data structures, logging ✓
- **Days 3-4**: Memory management, page fault handling
- **Days 5-6**: Network layer, message protocol
- **Days 7-8**: Consistency protocol, page migration
- **Day 9**: Synchronization primitives
- **Days 10-11**: Integration testing, debugging
- **Days 12-13**: Demo application with visualization
- **Day 14**: Documentation, presentation prep

## Technical Details

### Page Fault Handling
- Signal handler catches `SIGSEGV`
- Extracts fault address from `siginfo_t`
- Determines read vs write fault
- Requests page from owner node
- Updates permissions via `mprotect`

### Consistency Model
- **Release Consistency**: Memory ordering is only enforced at synchronization points (barriers/locks).
- **Protocol**: Single-Writer/Multiple-Reader invalidation-based protocol.
- **See [docs/CONSISTENCY.md](docs/CONSISTENCY.md) for full details and usage examples.**

### Network Protocol
- TCP for reliability
- Custom message format with header + payload
- Message types: PAGE_REQUEST, PAGE_REPLY, INVALIDATE, etc.
- Request queuing for concurrent access

## Design Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Page Size | 4KB | Standard page size, balance granularity/overhead |
| Transport | TCP | Reliability over performance for correctness |
| Consistency | Single-Writer | Simplest to implement correctly |
| Fault Handling | SIGSEGV | Only user-space option available |

## Performance Metrics

The system tracks:
- Page fault frequency (read vs write)
- Network transfer volume
- Page migration count
- Lock acquisition latency
- Barrier synchronization time

## Testing Strategy

1. **Unit Tests**: Individual component testing
2. **Integration Tests**: Multi-node scenarios
3. **Stress Tests**: High concurrency, rapid access
4. **Correctness Tests**: Sequential consistency validation

## Known Limitations

- Single-writer consistency (not MSI/MESI)
- No fault tolerance (node failures not handled)
- No page prefetching or optimization
- Limited scalability (tested up to 4 nodes)

## Future Enhancements

- [ ] MSI consistency protocol
- [ ] Page prefetching
- [ ] Adaptive page size
- [ ] Fault tolerance
- [ ] Performance optimizations (batching, compression)

## References

- Tanenbaum, "Distributed Systems: Principles and Paradigms" - DSM Chapter
- Li & Hudak, "Memory Coherence in Shared Virtual Memory Systems" (IVY)
- POSIX Signal Handling: `man 2 sigaction`
- Memory Protection: `man 2 mprotect`

## Authors

Operating Systems Semester Project - 2025

## License

Educational/Academic Use Only

# Game of Life - DSM Demo (C Implementation)

## Overview

This is a parallel implementation of Conway's Game of Life that demonstrates the User-Level DSM system in action. The implementation distributes the computation of a 2D cellular automaton grid across multiple nodes, with each node responsible for computing a subset of rows. The DSM system handles transparent data sharing and synchronization, making cross-partition boundary accesses seamless through page fault handling.

**Purpose:**

- Demonstrate DSM capabilities in a real parallel application
- Showcase automatic page migration on boundary accesses
- Visualize DSM overhead and synchronization patterns
- Provide measurable performance metrics (page faults, network transfers)

## Conway's Game of Life Rules

**Classic cellular automaton rules:**

- Any live cell with 2-3 live neighbors survives to the next generation
- Any dead cell with exactly 3 live neighbors becomes alive
- All other cells die or remain dead

**Grid topology:** Toroidal (wraps at edges)

**Neighborhood:** 8-neighbors (Moore neighborhood)

## DSM Integration Architecture

### Partitioning Strategy

**Row-based partitioning:**

- Grid divided into horizontal strips
- Each node owns a contiguous range of rows
- Even distribution: `rows_per_node = ceil(height / num_nodes)`

**Example (100x100 grid, 4 nodes):**

- Node 0: rows 0-24 (25 rows)
- Node 1: rows 25-49 (25 rows)
- Node 2: rows 50-74 (25 rows)
- Node 3: rows 75-99 (25 rows)

### Memory Layout

**Per-Node Allocation Design:**
Each node allocates its own partition using `dsm_malloc()`, creating truly distributed memory rather than a single shared allocation.

**Data structures:**

- `partitions_current[MAX_NODES]`: Pointers to each node's current generation buffer
- `partitions_next[MAX_NODES]`: Pointers to each node's next generation buffer
- Each partition: `uint8_t` array of size `(num_rows * grid_width)` bytes

**Memory allocation flow:**

1. **Sequential allocation phase:**

   - Nodes allocate in order (Node 0, then Node 1, etc.)
   - Barriers between each allocation ensure deterministic allocation indices
   - Node N allocates two buffers (current and next generation)
   - `dsm_malloc()` broadcasts `ALLOC_NOTIFY` to all nodes with base address

2. **SVAS retrieval:**
   - After all allocations complete, nodes call `dsm_get_allocation(node * 2)` and `dsm_get_allocation(node * 2 + 1)`
   - Retrieves pointers to other nodes' partitions at the same virtual addresses
   - Single Virtual Address Space ensures pointers are valid across all nodes

**Why per-node allocation?**

- Complies with Single-Writer/Multiple-Reader consistency protocol
- Each node has exclusive write access to its partition
- Other nodes read boundary rows, triggering page faults as designed
- Demonstrates SVAS (all nodes see same virtual addresses)

### Page Access Patterns

**Boundary row problem:**
Computing row `i` requires reading rows `i-1`, `i`, and `i+1` (with toroidal wrapping).

**Intra-partition accesses:**

- Node computes rows within its partition → local memory access via `PROT_READ | PROT_WRITE`
- No page faults

**Cross-partition accesses:**

- Node at boundary needs neighbor's row → triggers page fault
- DSM fetches page from owning node
- Permission set to `PROT_READ` (read-only copy)

**Example (Node 1 computing row 49):**

1. Read row 48 (own partition) → local access
2. Read row 49 (own partition) → local access
3. Read row 50 (Node 2's partition) → **page fault**
   - Send `PAGE_REQUEST(READ)` to Node 2
   - Receive `PAGE_REPLY` with page data
   - `mprotect(PROT_READ)` grants read access
   - Computation continues

**Page granularity:** 4KB pages contain multiple rows (e.g., 4096 bytes / 100 cells = ~41 rows), so one fault fetches many rows.

## Computation Flow

### Initialization Phase

**1. DSM Initialization:**

- Manager (Node 0) starts TCP server, waits for workers
- Workers connect and send `NODE_JOIN`
- All nodes initialize DSM context, install SIGSEGV handler

**2. State Initialization:**

- Calculate partition boundaries based on node ID
- Create `gol_state_t` structure with partition metadata

**3. Sequential Allocation:**

```
for each node i from 0 to num_nodes-1:
    if this_node == i:
        allocate my partition (current + next buffers)
    BARRIER(ALLOC_i)  // Wait for this node's allocation
```

**4. SVAS Retrieval:**

- All nodes retrieve pointers to all partitions
- Verify addresses match across nodes

**5. Pattern Initialization:**

- **Glider:** Node 0 initializes small moving pattern
- **R-pentomino:** Node 0 initializes methuselah pattern
- **Blinker:** Node 0 initializes oscillator
- **Random:** All nodes randomly populate their own partition (30% density default)

**6. Barrier:** Ensure all nodes see initialized grid before computation

### Main Generation Loop

**For each generation `g` from 0 to N:**

**Step 1: Compute Next Generation**

- Each node iterates over its rows: `[start_row, end_row)`
- For each cell `(r, c)`:
  - Count 8 neighbors using `count_neighbors()`
  - Neighbor accesses may trigger page faults on boundary rows
  - Apply Conway's rules: `apply_rules(current_state, neighbor_count)`
  - Write result to `partitions_next[my_node][r][c]`

**Step 2: Computation Barrier**

```
dsm_barrier(BARRIER_COMPUTE_BASE + gen, num_nodes)
```

- Ensures all nodes finish computing before any swap grids
- Prevents reading partially-updated data

**Step 3: Status Output (optional)**

- At display intervals, nodes acquire display lock
- Print generation number, live cell count, DSM stats
- Release lock

**Step 4: Swap Grids**

- Swap pointers: `partitions_current <-> partitions_next`
- All nodes perform swap simultaneously after barrier

**Step 5: Swap Barrier**

```
dsm_barrier(BARRIER_SWAP_BASE + gen, num_nodes)
```

- Ensures all nodes finish swap before next generation

### Cleanup Phase

**1. Final Barrier:**

- Synchronize before printing final statistics

**2. Free DSM Memory:**

- Each node calls `dsm_free()` on its partition buffers

**3. Finalize DSM:**

- Shutdown network connections
- Cleanup page tables and directory
- Uninstall signal handlers

## Synchronization Mechanisms

### Distributed Barriers

**Used at multiple points:**

- `BARRIER_ALLOC + i`: After each node's allocation (per-node barriers)
- `BARRIER_INIT`: After pattern initialization
- `BARRIER_COMPUTE_BASE + gen`: After each generation's computation
- `BARRIER_SWAP_BASE + gen`: After grid swap
- `BARRIER_FINAL`: Before final stats
- `BARRIER_CLEANUP`: Before resource cleanup

**Implementation:**

- Centralized on Manager (Node 0)
- Each node sends `BARRIER_ARRIVE` to manager
- Manager counts arrivals, broadcasts `BARRIER_RELEASE` when all arrived
- Nodes wait on local condition variable

**Why so many barriers?**

- Ensure deterministic allocation order (SVAS requirement)
- Prevent data races (reading partially-updated generation)
- Synchronize grid swaps
- Clean shutdown

### Distributed Lock

**Display lock:**

- ID: `LOCK_DISPLAY` (5000)
- Purpose: Serialize terminal output to prevent interleaved messages
- Usage: Acquire before printing stats, release after

**Implementation:**

- Centralized queue on Manager
- FIFO grant order
- Nodes send `LOCK_REQUEST`, wait for `LOCK_GRANT`

## DSM Usage Patterns

### Memory Operations

**Allocation:**

```c
// Each node allocates its partition
size_t partition_size = num_rows * grid_width * sizeof(uint8_t);
cell_t *current = dsm_malloc(partition_size);
cell_t *next = dsm_malloc(partition_size);
```

**Retrieval (SVAS):**

```c
// Other nodes get pointers
state->partitions_current[node_id] = dsm_get_allocation(node_id * 2);
state->partitions_next[node_id] = dsm_get_allocation(node_id * 2 + 1);
```

**Access:**

```c
// Get cell from any partition
cell_t* gol_get_cell(state, grid, row, col) {
    int owner = row / state->partition_num_rows[...];
    cell_t *partition = (grid == 0) ?
        state->partitions_current[owner] :
        state->partitions_next[owner];
    int local_row = row - state->partition_start_row[owner];
    return &partition[local_row * state->grid_width + col];
}
```

### Page Fault Triggers

**Designed fault points:**

- Boundary row accesses in `count_neighbors()`
- Node computing row 0 reads row `height-1` from last node
- Node computing row `end_row-1` reads row `end_row` from next node

**Fault frequency:**

- Depends on grid size and page size
- 100x100 grid, 4KB pages ≈ 41 rows per page
- Boundary accesses may hit same page multiple times (caching)

## Performance Characteristics

### Expected Metrics

**Page Faults:**

- **Read faults:** Dominate (boundary row reads)
- **Write faults:** Rare (each node only writes its own partition)
- Fault count depends on: grid size, partition count, page size

**Network Traffic:**

- Proportional to page faults (4KB per fault)
- Invalidations: rare (no write sharing)
- Barrier/lock messages: small overhead

**Synchronization:**

- Barriers: 2 per generation + initialization/cleanup
- Barrier latency: network RTT + manager processing
- Lock contention: minimal (only for display output)

### Scalability Factors

**Compute distribution:**

- Near-linear speedup for large grids (computation dominates)
- Communication overhead increases with more nodes

**Memory overhead:**

- Per-node: ~2 _ (grid_height / num_nodes) _ grid_width bytes
- DSM metadata: page tables, directory entries

**Network bottleneck:**

- Boundary page fetches on first access to each page
- Subsequent generations benefit from cached remote pages
- Barrier synchronization limits overall throughput

## Configuration Options

### Grid Parameters

- `--width WIDTH`: Grid width (default: 100)
- `--height HEIGHT`: Grid height (default: 100)
- `--generations N`: Number of generations (default: 100)
- `--display-interval N`: Print stats every N generations (default: 10)

### Pattern Selection

- `--pattern glider`: Small spaceship that moves diagonally
- `--pattern rpentomino`: Methuselah (stabilizes after 1103 gens)
- `--pattern blinker`: Period-2 oscillator
- `--pattern random`: Random 30% density (default)
- `--density D`: Density for random pattern (0.0-1.0)

### DSM Configuration

- `--node-id ID`: This node's ID (0 = manager)
- `--num-nodes N`: Total nodes (2-4)
- `--manager HOST`: Manager hostname for workers
- `--port PORT`: Base port number (default: 5000)
- `--log-level L`: DSM log verbosity (0=NONE, 3=INFO, 4=DEBUG)

### Example Command

**Manager (Node 0):**

```bash
./game_of_life --node-id 0 --num-nodes 2 --width 200 --height 200 \
               --generations 100 --pattern random --port 5000
```

**Worker (Node 1):**

```bash
./game_of_life --node-id 1 --num-nodes 2 --manager 127.0.0.1:5000 \
               --width 200 --height 200 --generations 100 --pattern random
```

## Output Format

### Progress Messages

**Initialization:**

```
[Node 0] Allocating my partition...
[Node 0] Waiting at BARRIER_ALLOC_0...
[Node 0] Retrieving other partitions...
[Node 0] Pattern initialized, starting computation
```

**Generation Stats (every display_interval):**

```
[Node 0] Gen 10: live=1234, faults=56 (+23), net=230KB (+92KB)
```

**Completion:**

```
[Node 0] === Final Statistics ===
Page Faults:       123 (read: 120, write: 3)
Pages Fetched:     115
Network:          460 KB sent, 470 KB received
Barrier Waits:    202
```

### Stats Tracking

**Per-generation deltas:**

- Page fault increase
- Network transfer increase
- Live cell count in this partition

**Cumulative totals:**

- Total faults, fetches, invalidations
- Lock acquisitions, barrier waits
- Network bytes sent/received

## Key Implementation Functions

### State Management (`gol_state.c`)

- `gol_state_init()`: Initialize state structure
- `gol_calculate_partition()`: Compute row boundaries per node
- `gol_allocate_partition()`: Allocate this node's memory via `dsm_malloc()`
- `gol_retrieve_partitions()`: Get pointers to other nodes' partitions via SVAS
- `gol_get_cell()`: Unified cell accessor (handles partition mapping)
- `gol_set_cell()`: Unified cell setter
- `gol_state_cleanup()`: Free resources

### Game Logic (`gol_rules.c`)

- `count_neighbors()`: Count 8-neighbors (triggers cross-partition faults)
- `apply_rules()`: Apply Conway's rules to determine next state
- `compute_generation()`: Main computation loop for this node's partition
- `initialize_pattern()`: Set up initial grid configuration
- `init_glider()`, `init_rpentomino()`, `init_blinker()`, `init_random_partition()`: Pattern generators

### Main Driver (`gol_main.c`)

- `initialize_dsm()`: Setup DSM context from configuration
- `parse_arguments()`: Command-line parsing
- `main()`: Orchestrates initialization → computation loop → cleanup

## Design Rationale

### Why Row-Based Partitioning?

**Advantages:**

- Locality: Cells in same row are adjacent in memory
- Minimal boundary surface: Only top/bottom rows cross partitions
- Cache-friendly: Iterating across columns has good spatial locality

**Alternatives considered:**

- Column-based: Same properties, arbitrary choice
- Block-based: More boundary surface, more page faults

### Why Per-Node Allocation?

**Alternative: Single shared allocation by Node 0:**

- Would violate SWMR: all nodes write to same allocation
- Requires complex locking or regions

**Current approach:**

- Clean ownership: each partition has one writer
- Demonstrates SVAS and distributed allocation
- Realistic multi-allocation scenario

### Why Double Buffering?

**Prevents read-after-write hazards:**

- Reading next-gen before all nodes finish computing → incorrect results
- Barriers enforce bulk-synchronous parallelism
- Swap is cheap (pointer reassignment)

### Why Toroidal Wrapping?

- Eliminates edge cases (no special boundary handling)
- Continuous interesting behavior (patterns wrap around)
- Symmetry across all cells

## Testing and Validation

### Correctness Verification

**1. Sequential baseline:**
Run same initial pattern on single node, compare final grid state

**2. Partition consistency:**
Verify boundary rows match between partitions after each generation

**3. Live cell conservation:**
Sum live cells across all partitions should match expected evolution

### Performance Testing

**1. Scalability:**

- Fixed grid size (e.g., 1000x1000), vary node count (1, 2, 4)
- Measure: total runtime, page faults, network traffic

**2. Grid size scaling:**

- Fixed node count (e.g., 4), vary grid (100², 200², 500²)
- Observe: fault rate, computation time

**3. Pattern comparison:**

- Glider: predictable, low activity
- Random: high activity, more faults
- Measure: faults per generation, network bytes

### Debugging Tips

**Enable debug logging:**

```bash
--log-level 4
```

- Shows DSM operations: page requests, invalidations, barriers

**Single node test:**

```bash
./game_of_life --node-id 0 --num-nodes 1 --generations 10
```

- No DSM communication, tests local computation

**Verify SVAS:**

- Check partition addresses printed during initialization
- Should be identical across all nodes

# Game of Life Visualization (Python)

## Overview

The DSM Visualizer is a Python-based real-time visualization tool built with Pygame that provides visual insight into the distributed Game of Life computation and DSM system behavior. It displays the evolving grid with partition ownership, page fault animations, and comprehensive DSM statistics.

**Purpose:**

- Visualize distributed computation in real-time
- Show partition boundaries and ownership via color coding
- Animate page faults as they occur
- Display DSM performance metrics (faults, network, synchronization)
- Support multiple operating modes (demo, live, replay)

**Framework:** Pygame for rendering, multithreaded process monitoring

## Architecture

### Operating Modes

**1. Demo Mode**

- Pure Python simulation of Game of Life
- No actual DSM processes
- Simulated page faults based on pattern
- Useful for visualization development and UI testing

**2. Live Mode**

- Launches actual Game of Life C processes (manager + workers)
- Monitors stdout for statistics updates
- Parses generationinfo and renders grid in real-time
- Shows true DSM behavior

**3. Replay Mode**

- Reads CSV statistics files from previous run
- Replays visualization at original timing
- Useful for analysis and presentation

### Component Structure

```
main.py
  ├─ Configuration (config.py)
  ├─ Data Sources
  │   ├─ ProcessMonitor (live mode)
  │   ├─ CSVReader (replay mode)
  │   └─ DemoSimulator (demo mode)
  ├─ Models
  │   ├─ GridState
  │   └─ DSMStats
  └─ Renderers
      ├─ PygameGridRenderer
      └─ StatsPanel
```

## Visual Design

### Color Scheme

**Partition ownership:** Each node assigned a distinct color

- Node 0: Sky Blue (`#4FACFE`)
- Node 1: Mint Green (`#43D9AD`)
- Node 2: Golden Yellow (`#FFD166`)
- Node 3: Coral Red (`#FF6B81`)

**Cell states:**

- **Alive cells:** Bright version of node color
- **Dead cells:** Dark version of node color
- Creates clear visual distinction while maintaining ownership identity

**UI elements:**

- Background: Dark navy (`#0F0F14`)
- Stats panel: Layered cards with subtle shadows
- Partition boundaries: Glowing lines with node accent color
- Page fault flash: Bright yellow (`#FFEB3B`) pulse animation

### Grid Rendering

**Cell visualization:**

- Each cell rendered as small rectangle with rounded corners
- Size: configurable (default 12x12 pixels)
- Borders: subtle, rounded for modern aesthetic
- Color: based on owner node and alive/dead state

**Partition boundaries:**

- Drawn at row boundaries between partitions
- Glow effect: multiple overlaid lines with decreasing opacity
- Color: accent color of partition owner
- Thickness: 2-3 pixels

**Page fault animation:**

- Flash entire row or specific cell
- Duration: ~10 frames (1 second at 10 FPS)
- Effect: bright yellow overlay with fade-out
- Helps visualize boundary accesses triggering faults

### Stats Panel

**Layout:** Right-side panel (320px width)

**Sections:**

1. **Header:**

   - Generation counter
   - Playback status (running/paused)
   - Simulation speed slider (demo mode)

2. **Per-Node Stats Cards:**

   - One card per node with node's accent color
   - Shows:
     - Live cells in partition
     - Page faults (total, read, write)
     - Network transfer (sent, received)
     - Barrier waits

3. **Global Stats Card:**

   - Total live cells across all nodes
   - Total page faults
   - Total network traffic
   - Average fault latency

4. **Control Help:**
   - Keyboard shortcuts legend
   - Space: pause/resume
   - S: step one generation
   - R: reset simulation
   - +/- : speed up/down

## Data Flow

### Live Mode Pipeline

**1. Process Launch:**

```
VisualizerMain
    ↓
GameOfLifeMonitor.start()
    ↓
Spawn manager process (node 0)
Spawn worker processes (nodes 1-N)
    ↓
Start output reader threads (one per process)
```

**2. Output Parsing:**

```
Process stdout → Reader thread → parse_line()
    ↓
Regex patterns match specific output formats
    ↓
Extract: generation, page_faults, network, barriers
    ↓
Create ProcessEvent object → queue
```

**3. Main Loop:**

```
while running:
    event = monitor.get_event(timeout=0.1)
    if event.type == GENERATION:
        update GridState
    elif event.type == PAGE_FAULTS:
        update DSMStats
        trigger fault animation

    render(grid, stats, generation)
    clock.tick(fps)
```

### Parsed Event Types

**GENERATION:**

- Extracted from: `[Node X] Gen Y: live=Z, ...`
- Data: `{generation: int, live_cells: int}`
- Action: Update grid state, increment generation counter

**PAGE_FAULTS:**

- Extracted from: `faults=X (+Y)`
- Data: `{total_faults: int, delta: int}`
- Action: Update stats, trigger fault flash if delta > 0

**NETWORK:**

- Extracted from: `net=XKB (+YKB)`
- Data: `{total_kb: int, delta_kb: int}`
- Action: Update network statistics

**BARRIER:**

- Extracted from: `Waiting at BARRIER_X` or `Passed BARRIER_X`
- Data: `{barrier_id: int, action: str}`
- Action: Increment barrier wait count

**INIT:**

- Extracted from: `Pattern initialized`
- Action: Mark initialization complete

**COMPLETE:**

- Extracted from: `Computation complete`
- Action: Stop monitoring, allow clean exit

### Demo Mode Pipeline

**No external processes:**

```
DemoSimulator
    ↓
Maintains GridState in memory
    ↓
Each tick:
    - Compute next generation (Python Game of Life rules)
    - Randomly trigger simulated page faults at boundaries
    - Update stats with artificial metrics
    ↓
Render updated state
```

**Advantages:**

- Fast development iteration
- No DSM compilation needed
- Controllable fault simulation

### Replay Mode Pipeline

**Read from CSV files:**

```
CSVStatsReader
    ↓
Load dsm_stats_node0.csv, dsm_stats_node1.csv, ...
    ↓
Parse rows: timestamp, generation, page_faults, network, ...
    ↓
Playback at original intervals
    ↓
Render historical data
```

**Use cases:**

- Post-run analysis
- Create presentation demos
- Compare multiple runs side-by-side

## Process Monitoring

### Subprocess Management

**Manager process:**

```bash
./game_of_life --node-id 0 --num-nodes N --width W --height H \
               --generations G --pattern P --port 5000
```

**Worker processes:**

```bash
./game_of_life --node-id i --num-nodes N --manager 127.0.0.1:5000 \
               --width W --height H --generations G --pattern P
```

**Process control:**

- `subprocess.Popen()` with `stdout=PIPE`, `stderr=PIPE`
- Non-blocking read via separate threads
- Clean shutdown via `SIGTERM`, then `SIGKILL` if needed

### Output Stream Parsing

**Regex patterns:**

```python
# Generation update
r'\[Node (\d+)\] Gen (\d+): live=(\d+)'

# Page faults
r'faults=(\d+) \(\+(\d+)\)'

# Network
r'net=(\d+)KB \(\+(\d+)KB\)'

# Barriers
r'Waiting at BARRIER_(\d+)'
```

**Thread-safe queue:**

- Each reader thread pushes `ProcessEvent` objects to shared queue
- Main thread pops events and updates UI

**Synchronization:**

- Track minimum generation across all nodes
- Only render when all nodes at same generation (prevents tearing)

## Rendering Pipeline

### Frame Rendering Sequence

**1. Handle Input:**

- Process Pygame events (quit, keyboard)
- Check for pause/unpause, step, reset

**2. Update State:**

- Fetch new events from monitor queue
- Update `GridState` with latest cell data
- Update `DSMStats` with latest metrics

**3. Clear Screen:**

- Fill with dark background color

**4. Draw Grid:**

- Iterate all cells `(r, c)`
- Determine owner from partition boundaries
- Choose color: `NODE_ALIVE_COLORS[owner]` if alive else `NODE_DEAD_COLORS[owner]`
- Draw rounded rectangle at `(c * cell_size, r * cell_size)`

**5. Draw Partition Boundaries:**

- For each partition boundary row
- Draw glowing line with multiple passes (opacity layers)

**6. Draw Fault Highlights:**

- For each active fault animation
- Draw bright overlay at fault row/cell
- Decrement animation counter

**7. Draw Stats Panel:**

- Render background card
- Render per-node stats with colored headers
- Render global summary
- Render control hints

**8. Display Flip:**

- `pygame.display.flip()` to show rendered frame

**9. Throttle:**

- `clock.tick(fps)` to maintain target frame rate

### Animation System

**Fault animation:**

- Stored in dictionary: `{(row, col): remaining_frames}`
- Each frame: decrement counter, remove if zero
- Render as bright yellow overlay with alpha fade

**Triggering:**

- On `PAGE_FAULTS` event with `delta > 0`
- Call `renderer.trigger_fault_at_row(boundary_row, grid_width)`
- Adds all cells in row to animation dict

**Smooth fading:**

- Opacity = `(remaining_frames / FAULT_FLASH_FRAMES) * 255`
- Creates smooth fade-out effect

## User Interaction

### Keyboard Controls

**Playback:**

- `Space`: Pause/Resume simulation
- `S`: Step one generation (when paused)
- `R`: Reset to initial state (demo mode only)

**Display:**

- `+/-`: Increase/decrease simulation speed (demo mode)
- `Q` or `ESC`: Quit application

**Window:**

- Support for window close button
- Clean shutdown of all processes

### Real-Time Feedback

**Visual indicators:**

- Pause overlay: Semi-transparent "PAUSED" text
- Generation counter updates
- Stats numbers change in real-time
- Fault flashes provide immediate visual feedback

**Responsiveness:**

- UI runs at 10-60 FPS depending on mode
- Input handled every frame
- Non-blocking process monitoring (events queued)

## Configuration

### Command-Line Arguments

**Grid settings:**

```bash
--width WIDTH         # Grid width (default: 60)
--height HEIGHT       # Grid height (default: 60)
--cell-size SIZE      # Cell pixel size (default: 12)
```

**DSM settings:**

```bash
--num-nodes N         # Number of nodes (default: 2)
--executable PATH     # Path to game_of_life binary (live mode)
--generations N       # Number of generations (live mode)
--pattern PATTERN     # Initial pattern name (live mode)
```

**Mode selection:**

```bash
--demo                # Run in demo mode (default)
--live                # Launch real processes
--replay DIR          # Replay from stats directory
```

**Display:**

```bash
--fps FPS             # Target frame rate (default: 10)
--no-stats            # Hide stats panel
```

### Example Invocations

**Demo mode (quick visualization):**

```bash
python main.py --demo --width 100 --height 100 --num-nodes 4
```

**Live mode (real DSM):**

```bash
python main.py --live --executable ../game_of_life/build/game_of_life \
               --num-nodes 2 --width 200 --height 200 --generations 100 \
               --pattern random
```

**Replay mode (analyze previous run):**

```bash
python main.py --replay ../stats/run_20251127_1200 --width 200 --height 200
```

## Performance Considerations

### Rendering Optimization

**Cell batching:**

- Could optimize by drawing all cells of same color in batch
- Current implementation: individual `pygame.draw.rect()` per cell
- Trade-off: simplicity vs. performance

**Dirty rectangles:**

- Could track changed cells and only redraw those regions
- Full redraw is acceptable for small grids (<500x500) at 10 FPS

**Frame rate:**

- 10 FPS sufficient for observing computation (100ms per generation)
- Higher FPS for smoother animations but unnecessary CPU usage

### Process Monitoring

**Thread overhead:**

- One thread per process for stdout reading
- Minimal CPU (blocking `readline()`)
- Queue size bounded (events consumed quickly)

**Event latency:**

- ~10-100ms from process output to visualization
- Acceptable for real-time monitoring

## Extensibility

### Adding New Modes

**Custom data source:**

1. Implement interface with `get_event(timeout)` method
2. Return `ProcessEvent` objects
3. Plug into main loop

**Example:** WebSocket data source for remote monitoring

### Custom Renderers

**Alternative visualization:**

1. Subclass or reimplement renderer
2. Implement `render(grid, stats, generation)`
3. Could use different library (matplotlib, tkinter, web canvas)

**Example:** 3D height-map rendering of living cells

### Pattern Extensions

**Add new patterns in demo mode:**

1. Extend `DemoSimulator.initialize_pattern()`
2. Define initial cell configuration
3. Register in pattern dictionary

## Debugging and Testing

### Visualization Testing

**Demo mode:**

- Known patterns (glider) should move predictably
- Can verify Game of Life rules implementation

**Live mode:**

- Compare stats panel with C program output
- Verify fault counts match expected behavior

### Process Monitoring Testing

**Manual testing:**

1. Run game_of_life manually, capture stdout
2. Feed to parser, verify events extracted correctly
3. Check edge cases (malformed output, interleaving)

**Simulation:**

- Generate synthetic stdout stream
- Verify parser handles all event types

### Performance Profiling

**Frame timing:**

```python
import cProfile
cProfile.run('main()')
```

- Identify bottlenecks in rendering

**Memory usage:**

- Grid state: `O(width * height)` bytes
- Stats: constant size
- Animation dict: size proportional to fault frequency
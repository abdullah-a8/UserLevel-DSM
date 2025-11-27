# User-Level DSM Architecture

This document describes the architecture and implementation of the User-Level Distributed Shared Memory (DSM) system.

## Overview

This is a user-space implementation of Distributed Shared Memory that provides the illusion of a single shared address space across multiple machines. The system uses standard POSIX APIs (`mmap`, `mprotect`, `sigaction`) and TCP sockets to implement page-based memory coherence without requiring any kernel modifications.

**Key Design Principles:**

- **Page-level granularity**: 4KB pages as the unit of sharing
- **Single Virtual Address Space (SVAS)**: All nodes map allocations at the same virtual addresses
- **Single-Writer/Multiple-Reader**: Invalidation-based consistency protocol
- **Centralized coordination**: Manager node (Node 0) handles lock/barrier coordination and directory management
- **Lazy page migration**: Pages migrate on-demand via page faults

## System Architecture

### Node Roles

**Manager Node (Node 0):**

- Runs TCP server accepting connections from workers
- Maintains centralized page directory (ownership and sharer tracking)
- Coordinates distributed locks and barriers
- Broadcasts allocation notifications
- Replicates state to primary backup for fault tolerance

**Primary Backup Node (Node 1):**

- Functions as worker for application workload
- Maintains shadow directory, locks, and barriers
- Receives asynchronous state replication from manager
- Promotes to manager on Node 0 failure
- Pre-binds to manager port for seamless failover

**Worker Nodes (Node 2+):**

- Connect to manager via TCP client
- Local page table tracking
- Send page requests and receive data
- Participate in synchronization
- Redirect to new manager on failover

### Core Components

```
┌─────────────────────────────────────────────────────────┐
│                    Application Layer                     │
│              (dsm_malloc, locks, barriers)               │
└──────────────────┬──────────────────────────────────────┘
                   │
┌──────────────────┴──────────────────────────────────────┐
│                   DSM API (dsm.h)                        │
│         dsm_init, dsm_malloc, dsm_lock, dsm_barrier      │
└──────────────────┬──────────────────────────────────────┘
                   │
      ┌────────────┼────────────┐
      │            │            │
┌─────▼─────┐ ┌───▼────┐ ┌────▼──────┐
│  Memory   │ │ Fault  │ │   Sync    │
│  Manager  │ │Handler │ │ Primitives│
└─────┬─────┘ └───┬────┘ └────┬──────┘
      │           │            │
      └───────────┼────────────┘
                  │
        ┌─────────▼─────────┐
        │   Consistency     │
        │ (Page Migration + │
        │    Directory)     │
        └─────────┬─────────┘
                  │
        ┌─────────▼─────────┐
        │   Network Layer   │
        │ (TCP + Protocol)  │
        └───────────────────┘
```

## Memory Management

### Allocation (`dsm_malloc`)

**Process Flow:**

1. Round size up to page boundary
2. Allocate memory region using `mmap(PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS)`
   - Initially no access permissions (triggers faults on first access)
3. Create page table tracking all pages in allocation
4. Assign globally unique page IDs: `(node_id << 32) | (allocation_index << 16) | page_offset`
5. Set this node as owner in local page table and directory
6. Broadcast `ALLOC_NOTIFY` to all nodes with base address and page range
7. Wait for `ALLOC_ACK` from all nodes before returning

**Workers receiving ALLOC_NOTIFY:**

1. Create `mmap` at the **same virtual address** (SVAS requirement)
2. Create remote page table with received page IDs
3. Mark pages as owned by remote node
4. Send `ALLOC_ACK` back

**C Functions Used:**

- `mmap()`: Allocate virtual address space
- `munmap()`: Free pages on `dsm_free()`
- Page tables track metadata per page

### Page Tables

**Structure:** Array-based with linear lookup

- **Multiple tables**: System supports up to 32 separate allocations
- **Globally tracked**: `ctx->page_tables[0..31]`

**Page Entry (`page_entry_t`):**

- `page_id`: Globally unique identifier
- `local_addr`: Virtual address on this node
- `owner`: Node ID of current owner
- `state`: `INVALID` / `READ_ONLY` / `READ_WRITE`
- `version`: Version counter for consistency
- `request_pending`: Flag to prevent duplicate fetches (request queuing)
- `entry_lock`: Per-page mutex for fine-grained locking
- `ready_cv`: Condition variable for waiting threads
- `num_waiting_threads`: Queue depth tracking

**Key Operations:**

- `page_table_lookup_by_addr()`: Find page entry from virtual address
- `page_table_lookup_by_id()`: Find page entry from global page ID
- `page_table_set_state()`: Update page state
- `page_table_acquire/release()`: Reference counting for safe concurrent access

## Page Fault Handling

### Signal Handler Installation

**Initialization:**

```c
struct sigaction sa;
sa.sa_flags = SA_SIGINFO;
sa.sa_sigaction = dsm_fault_handler;
sigaction(SIGSEGV, &sa, &old_sa);
```

**C Functions Used:**

- `sigaction()`: Install custom SIGSEGV handler
- `signal()`: Restore default handler on unrecognized faults

### Fault Detection and Handling

**Flow:**

1. Hardware generates page fault → kernel delivers `SIGSEGV`
2. Signal handler `dsm_fault_handler()` invoked
3. Extract fault address from `siginfo_t->si_addr`
4. Determine read vs. write fault from `ucontext_t->uc_mcontext.gregs[REG_ERR]`
   - Bit 1 (0x2) set → write fault
   - Otherwise → read fault
5. Search all page tables for matching page entry
6. If not in DSM region → re-raise SIGSEGV with default handler
7. Dispatch to `handle_read_fault()` or `handle_write_fault()`

### Read Fault Handler

**State Transition:** `INVALID` → `READ_ONLY`

1. Query directory for current owner
2. Check request queuing:
   - If another thread already fetching → wait on `entry->ready_cv`
   - Otherwise mark `request_pending = true`
3. Send `PAGE_REQUEST(READ)` to owner
4. Wait for `PAGE_REPLY` with page data
5. Copy data to local page
6. Update permissions: `mprotect(addr, PAGE_SIZE, PROT_READ)`
7. Update state to `READ_ONLY`
8. Notify directory to add this node as reader
9. Wake waiting threads via `pthread_cond_broadcast()`

**C Functions Used:**

- `mprotect(PROT_READ)`: Grant read-only access

### Write Fault Handler

**State Transitions:**

- `INVALID` → `READ_WRITE`
- `READ_ONLY` → `READ_WRITE`

1. Query directory for owner
2. Check request queuing
3. Query owner for complete sharer list (handles nodes that fetched READ without directory update)
4. Send `PAGE_REQUEST(WRITE)` to owner
5. Owner sends `INVALIDATE` to all sharers
6. Wait for `INVALIDATE_ACK` from all sharers
7. Owner sends `PAGE_REPLY` with data
8. Update local page with data
9. Update permissions: `mprotect(addr, PAGE_SIZE, PROT_READ | PROT_WRITE)`
10. Update state to `READ_WRITE`, set this node as owner
11. Update directory ownership
12. Wake waiting threads

**C Functions Used:**

- `mprotect(PROT_READ | PROT_WRITE)`: Grant read-write access

## Consistency Protocol

### Page Directory

**Implementation:** Hash table with chaining (100K buckets)

- Resides on manager node (Node 0)
- Workers query via `DIR_QUERY` / `DIR_REPLY` messages

**Directory Entry (`directory_entry_t`):**

- `page_id`: Page identifier
- `owner`: Node with write access
- `sharers`: List of nodes with read-only copies (up to 16)
- `lock`: Per-entry mutex

**Operations:**

- `directory_lookup()`: Get current owner
- `directory_add_reader()`: Add node to sharer list
- `directory_set_writer()`: Set new owner, return nodes to invalidate
- `directory_clear_sharers()`: Remove all sharers after invalidation

### Page Migration Protocol

**Read Access:**

1. Requester → Query directory → Get owner
2. Requester → `PAGE_REQUEST(READ)` → Owner
3. Owner → Add to local sharer tracking
4. Owner → `PAGE_REPLY` with data → Requester
5. Requester → Add as reader to directory
6. Requester sets page to `READ_ONLY`

**Write Access:**

1. Requester → Query directory → Get owner + sharers
2. Requester → `PAGE_REQUEST(WRITE)` → Owner
3. Owner → `INVALIDATE` → All sharers (from local tracking + directory)
4. Sharers → Set page to `INVALID` via `mprotect(PROT_NONE)`
5. Sharers → `INVALIDATE_ACK` → Owner
6. Owner waits for all ACKs (tracked via `pending_inv_acks`)
7. Owner → `PAGE_REPLY` → Requester
8. Requester → Update directory ownership
9. Requester sets page to `READ_WRITE`

**Sharer Tracking Fix:**
The implementation uses dual tracking:

- **Directory sharers**: Nodes that sent explicit read requests
- **Owner's local tracking**: All nodes that got READ access (including those from write downgrades)
- Before invalidation, requester queries owner for complete list via `SHARER_QUERY` / `SHARER_REPLY`

### Invalidation

On receiving `INVALIDATE`:

1. Find page entry in local page table
2. Set permission: `mprotect(addr, PAGE_SIZE, PROT_NONE)`
3. Update state to `INVALID`
4. Send `INVALIDATE_ACK` back

**C Functions Used:**

- `mprotect(PROT_NONE)`: Revoke all access

## Network Layer

### Connection Management

**Manager:**

- `socket()` + `bind()` + `listen()` on configured port
- Separate accept thread handles incoming connections
- Stores connections in `ctx->network.nodes[]` after `NODE_JOIN`

**Workers:**

- `socket()` + `connect()` to manager
- Send `NODE_JOIN` with node ID

**Dispatcher Thread:**

- Runs `poll()` on all connected sockets
- Reads messages via `network_recv()` and dispatches to handlers
- Separate thread prevents blocking

**C Functions Used:**

- `socket()`, `bind()`, `listen()`, `accept()`, `connect()`
- `send()`, `recv()`: TCP byte stream transmission
- `poll()`: Monitor multiple sockets for incoming data

### Message Protocol

**Message Structure:**

```c
typedef struct {
    msg_header_t header;     // Magic, type, length, sender, seq_num
    union {
        page_request_payload_t;
        page_reply_payload_t;   // Includes 4KB page data
        invalidate_payload_t;
        lock_request_payload_t;
        barrier_arrive_payload_t;
        // ... 15+ message types
    } payload;
} message_t;
```

**Serialization:**

- Fixed-size headers
- Variable-size payloads (largest is `PAGE_REPLY` with 4KB data)
- Send header first, then payload
- Magic number `0xDEADBEEF` for validation

**Key Message Types:**

- `PAGE_REQUEST` / `PAGE_REPLY`: Page migration
- `INVALIDATE` / `INVALIDATE_ACK`: Coherence
- `LOCK_REQUEST` / `LOCK_GRANT` / `LOCK_RELEASE`: Locks
- `BARRIER_ARRIVE` / `BARRIER_RELEASE`: Barriers
- `ALLOC_NOTIFY` / `ALLOC_ACK`: SVAS allocation
- `DIR_QUERY` / `DIR_REPLY`: Directory lookup
- `SHARER_QUERY` / `SHARER_REPLY`: Get complete sharer list
- `HEARTBEAT`: Failure detection
- `NODE_JOIN` / `NODE_FAILED`: Membership
- `STATE_SYNC_DIR` / `STATE_SYNC_LOCK` / `STATE_SYNC_BARRIER`: Hot backup replication
- `MANAGER_PROMOTION`: Notify workers of new manager
- `RECONNECT_REQUEST`: Worker reconnection after failover

**Request/Response Tracking:**

- Sequence numbers for debugging
- Timeout-based handling (5-30 seconds depending on operation)
- Retry logic with exponential backoff (up to 3 retries)

## Synchronization Primitives

### Distributed Locks

**Architecture:** Centralized lock manager on Node 0

**Acquisition Flow:**

1. Node calls `dsm_lock_acquire(lock)`
2. Send `LOCK_REQUEST(lock_id)` to manager
3. Manager queues request in FIFO order
4. When lock available, manager sends `LOCK_GRANT` to requester
5. Requester wakes from `pthread_cond_wait()` and returns

**Release Flow:**

1. Node calls `dsm_lock_release(lock)`
2. Send `LOCK_RELEASE(lock_id)` to manager
3. Manager dequeues next waiter and sends `LOCK_GRANT` to them

**Data Structures:**

- `dsm_lock_t`: Client-side handle with condition variable
- Manager maintains queue of waiting nodes per lock

### Distributed Barriers

**Architecture:** Centralized barrier manager on Node 0

**Synchronization Flow:**

1. Each node calls `dsm_barrier(barrier_id, num_participants)`
2. Send `BARRIER_ARRIVE(barrier_id)` to manager
3. Wait on local condition variable
4. Manager counts arrivals
5. When all participants arrived, manager broadcasts `BARRIER_RELEASE` to all
6. Nodes wake from `pthread_cond_wait()` and continue

**Data Structures:**

- `dsm_barrier_t`: Arrival counter, participant list, condition variable
- Manager tracks per-barrier state in `ctx->barrier_mgr.barriers[]`

## Concurrency and Thread Safety

### Locking Hierarchy

**Global locks:**

1. `ctx->lock`: Protects DSM context, node list, page table array
2. `ctx->stats_lock`: Protects statistics counters
3. `ctx->allocation_lock`: Serializes `dsm_malloc()` calls

**Per-structure locks:**

- `page_table->lock`: Protects page table operations
- `entry->entry_lock`: Per-page fine-grained locking
- `network.pending_lock`: Protects pending socket array
- `directory->lock`: Protects directory hash table
- `entry->lock`: Per-directory-entry lock

**Ordering:** Always acquire global before per-structure locks

### Request Queuing

**Problem:** Multiple threads faulting on same page → duplicate network requests

**Solution:** First thread to fault sets `entry->request_pending = true`

- Subsequent threads increment `num_waiting_threads` and wait on `entry->ready_cv`
- First thread performs fetch, stores result in `entry->fetch_result`
- On completion, broadcasts to wake all waiters
- Waiters check result and either succeed or retry

## Fault Tolerance

### Hot Backup Architecture

The system implements **hot backup failover** with Node 1 as primary backup for manager (Node 0).

**Key Components:**

- **Shadow Directory**: Node 1 maintains replicated copy of page directory (100K buckets)
- **Shadow Locks**: Replicated state for all distributed locks (256 max)
- **Shadow Barriers**: Replicated state for all barriers (256 max)
- **Promotion Lock**: Prevents split-brain scenarios during failover
- **Backup Server Socket**: Pre-bound to manager port for instant takeover

### State Replication

**Manager → Backup Synchronization:**

After each state-modifying operation, manager asynchronously replicates to Node 1:

1. **Directory Operations** (in `directory.c`):
   - `directory_add_reader()` → Send `STATE_SYNC_DIR` with page_id, owner, sharers
   - `directory_set_writer()` → Send `STATE_SYNC_DIR` with updated ownership
   - `directory_clear_sharers()` → Send `STATE_SYNC_DIR` with empty sharer list
   - `directory_set_owner()` → Send `STATE_SYNC_DIR` with new owner

2. **Lock Operations**:
   - Lock acquire/release → Send `STATE_SYNC_LOCK` with holder and waiter queue

3. **Barrier Operations**:
   - Barrier arrive → Send `STATE_SYNC_BARRIER` with arrival count and generation

**Replication Properties:**

- **Asynchronous**: Non-blocking to avoid performance impact
- **Sequenced**: Each update carries `sync_seq` for ordering
- **Conditional**: Only when manager is Node 0 and not promoted backup
- **Single-target**: Only to Node 1 (primary backup)

### Failover Protocol

**Detection Phase:**

1. Heartbeat thread detects Node 0 failure (missed heartbeats exceed threshold)
2. Node 1 checks `is_primary_backup` flag
3. Acquires `promotion_lock` to prevent concurrent promotions

**Promotion Phase:**

1. Node 1 swaps shadow directory to active: `set_page_directory(backup_directory)`
2. Sets flags: `is_promoted = true`, `current_manager = 1`
3. Activates pre-bound backup server socket to accept connections
4. Broadcasts `MANAGER_PROMOTION` to all workers with new manager ID and timestamp

**Worker Reconnection Phase:**

1. Workers receive `MANAGER_PROMOTION` notification
2. Update local `current_manager` tracking to Node 1
3. Send `RECONNECT_REQUEST` with last sequence seen
4. Redirect all directory queries to Node 1
5. Retry pending operations with new manager

**Transparent Recovery:**

- Page fetch operations (`fetch_page_read/write`) detect manager timeout
- Check if Node 0 failed via `ctx->network.nodes[0].is_failed`
- Call `wait_for_manager_reconnection(5000ms)` polling for promotion
- Retry operations automatically with promoted manager
- No application-level intervention required

### Failure Handling

**Heartbeat Mechanism:**

- Separate thread sends periodic `HEARTBEAT` messages
- Tracks `last_heartbeat_time` and `missed_heartbeats` per node
- After threshold, mark node as failed via `is_failed` flag
- Triggers promotion logic if failed node is manager

**Directory Recovery:**

**On worker node failure:**

- `directory_handle_node_failure()`: Remove from all sharer lists, mark owned pages as orphaned
- `directory_reclaim_ownership()`: Requester can claim ownership on timeout

**On manager failure:**

- Primary backup promotes to manager role
- Workers redirect to Node 1 via `current_manager` tracking
- Pending operations retry with new manager after promotion completes

**Timeout Handling:**

- Page requests timeout after 5-30 seconds
- Detects manager failure and waits for promotion before retry
- Retries up to 3 times with exponential backoff
- Update stats: `ctx->stats.timeouts++`, `ctx->stats.network_failures++`

## Performance Features

### Request Queuing (Task 8.1)

Prevents thundering herd on same page

### Performance Logging (Task 8.6)

- `ctx->stats`: Counters for faults, transfers, invalidations
- Latency tracking: `total_fault_latency_ns`, `max_fault_latency_ns`
- CSV export via `dsm_perf_export_stats()`

### Statistics

```c
typedef struct {
    uint64_t page_faults;           // Total
    uint64_t read_faults;           // Read-only
    uint64_t write_faults;          // Exclusive
    uint64_t pages_fetched;         // Received
    uint64_t pages_sent;            // Sent
    uint64_t invalidations_sent;
    uint64_t network_bytes_sent;
    uint64_t lock_acquires;
    uint64_t barrier_waits;
    // ... performance metrics
} dsm_stats_t;
```

## Critical System Functions Summary

### Memory Management

- `mmap(PROT_NONE)`: Allocate pages with no initial access
- `munmap()`: Free pages
- `mprotect(PROT_NONE/READ/READ|WRITE)`: Change page permissions

### Signal Handling

- `sigaction(SIGSEGV)`: Install page fault handler
- `siginfo_t->si_addr`: Extract fault address
- `ucontext_t->uc_mcontext.gregs[REG_ERR]`: Determine read vs. write fault

### Network I/O

- `socket()`: Create TCP socket
- `bind()`, `listen()`, `accept()`: Server setup
- `connect()`: Client connection
- `send()`, `recv()`: Message transmission
- `poll()`: Multiplex socket I/O

### Thread Synchronization

- `pthread_mutex_lock/unlock()`: Mutual exclusion
- `pthread_cond_wait/signal/broadcast()`: Condition variables for page arrival, lock grants, barrier release
- `pthread_create()`: Dispatcher and heartbeat threads

### Time/Retry

- `clock_gettime(CLOCK_REALTIME)`: Timeout calculations
- `pthread_cond_timedwait()`: Bounded waiting
- `usleep()`: Exponential backoff between retries

## Data Flow Example: Write Fault

```
Thread accesses page → SIGSEGV → Handler extracts address
                                       ↓
                        Lookup page entry in page_tables[]
                                       ↓
                        Read REG_ERR → Detect write fault
                                       ↓
                        handle_write_fault(addr)
                                       ↓
                DIR_QUERY → Manager → DIR_REPLY (owner=Node X)
                                       ↓
                SHARER_QUERY → Node X → SHARER_REPLY (nodes Y,Z)
                                       ↓
                PAGE_REQUEST(WRITE) → Node X
                                       ↓
                Node X → INVALIDATE → Nodes Y,Z
                                       ↓
                Nodes Y,Z → mprotect(PROT_NONE) + INVALIDATE_ACK
                                       ↓
                Node X waits for all ACKs (pending_inv_acks)
                                       ↓
                Node X → PAGE_REPLY (4KB data) → Requester
                                       ↓
                Requester: memcpy to local_addr
                          mprotect(PROT_READ|PROT_WRITE)
                          Update state to READ_WRITE
                          directory_set_owner()
                                       ↓
                Wake waiting threads → Return to application
```

## Key Design Choices

| Aspect             | Choice                        | Rationale                                                         |
| ------------------ | ----------------------------- | ----------------------------------------------------------------- |
| Page size          | 4KB                           | Standard OS page size, hardware support for faults                |
| Consistency        | Single-Writer/Multiple-Reader | Simplest invalidation protocol, avoids write conflicts            |
| Directory          | Centralized on Node 0         | Simpler than distributed directory, acceptable for small clusters |
| Locks/Barriers     | Centralized on Node 0         | Easier implementation, single point of coordination               |
| Fault tolerance    | Hot backup (Node 1)           | Fast failover with replicated state, no checkpoint overhead       |
| Replication        | Asynchronous to Node 1        | Low latency impact, acceptable staleness for manager failover     |
| Network            | TCP                           | Reliability over raw performance                                  |
| Virtual addresses  | SVAS (same on all nodes)      | Simplifies pointer sharing, required for true DSM                 |
| Fault detection    | `SIGSEGV` signal handler      | Only user-space mechanism available                               |
| Permission control | `mprotect()`                  | OS-provided page protection, triggers hardware faults             |
| Request queuing    | Per-page with CV              | Prevents duplicate requests, wakes all waiters                    |

## Limitations

- **Scalability**: Centralized directory and lock manager limit cluster size
- **Single-writer**: No write-write sharing (MSI/MESI would be more sophisticated)
- **Single backup**: Only Node 1 serves as backup; no failover for backup itself
- **Replication lag**: Asynchronous replication may lose in-flight state during failure
- **Performance**: Network latency dominates (no prefetching or adaptive policies)
- **Max allocations**: 32 per node
- **Max sharers**: 16 per page

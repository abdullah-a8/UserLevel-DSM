# DSM Architecture Documentation

## System Architecture

### High-Level Overview

```
┌─────────────────────────────────────────────────────────────┐
│                    Application Layer                        │
│  (User programs using DSM API: malloc, locks, barriers)    │
└────────────────────┬────────────────────────────────────────┘
                     │
┌────────────────────┴────────────────────────────────────────┐
│                    DSM API Layer                            │
│  dsm_init(), dsm_malloc(), dsm_lock_*(), dsm_barrier()     │
└────┬────────┬──────────┬──────────┬─────────────────────────┘
     │        │          │          │
┌────▼────┐ ┌▼────┐ ┌───▼───┐ ┌────▼─────┐
│ Memory  │ │ Fault│ │Network│ │   Sync   │
│ Manager │ │Handler│ │ Layer │ │ Primitives│
└────┬────┘ └┬─────┘ └───┬───┘ └────┬─────┘
     │       │           │          │
┌────▼───────▼───────────▼──────────▼─────────────────────────┐
│              Operating System (Linux)                        │
│  mmap, mprotect, sigaction, socket, send/recv               │
└──────────────────────────────────────────────────────────────┘
```

## Component Breakdown

### 1. Memory Manager (`src/memory/`)

**Responsibilities:**
- Allocate DSM regions using `mmap`
- Maintain page table mapping virtual addresses to pages
- Track page ownership (which node owns which page)
- Manage page permissions using `mprotect`

**Key Data Structures:**
```c
page_table_t     - Maps virtual addresses to page entries
page_entry_t     - Stores page state, owner, version
```

**Key Functions:**
```c
dsm_malloc()     - Allocate DSM memory
page_table_*()   - Page table operations
set_page_permission() - Change page protections
```

### 2. Page Fault Handler (`src/memory/`)

**Responsibilities:**
- Catch page faults via `SIGSEGV` signal
- Determine fault type (read vs write)
- Coordinate page fetch from remote node
- Update page permissions after fetch

**Signal Handler Flow:**
```
SIGSEGV triggered
    ↓
Extract fault address (si_addr)
    ↓
Lookup page in page table
    ↓
Determine fault type (read/write)
    ↓
┌───────────────┐
│ Read Fault?   │
└───┬───────┬───┘
    │       │
   YES      NO (Write Fault)
    │       │
    ▼       ▼
Fetch page  Fetch page + Invalidate others
Read-only   Read-write
    │       │
    └───┬───┘
        ↓
Update mprotect permissions
        ↓
Return from signal handler
```

### 3. Network Layer (`src/network/`)

**Responsibilities:**
- TCP server for accepting connections
- TCP client for connecting to peers
- Message serialization/deserialization
- Reliable send/receive with error handling

**Message Format:**
```
┌──────────────────────────────────────┐
│      Message Header (32 bytes)       │
├──────────────────────────────────────┤
│ Magic (4B) | Type (4B) | Length (4B) │
│ Sender (4B) | Seq# (8B) | Reserved   │
├──────────────────────────────────────┤
│         Payload (variable)           │
│  (depends on message type)           │
└──────────────────────────────────────┘
```

**Message Types:**
- `PAGE_REQUEST`: Request page from owner
- `PAGE_REPLY`: Send page data
- `INVALIDATE`: Invalidate page copy
- `INVALIDATE_ACK`: Acknowledge invalidation
- `LOCK_REQUEST/GRANT/RELEASE`: Lock protocol
- `BARRIER_ARRIVE/RELEASE`: Barrier protocol

### 4. Consistency Protocol (`src/consistency/`)

**Responsibilities:**
- Implement Single-Writer/Multiple-Reader protocol
- Track page ownership globally
- Handle invalidations on write access
- Queue concurrent requests

**Protocol State Machine:**
```
Page States (per node):
┌─────────────────────────────────────────┐
│  INVALID  →  READ_ONLY  →  READ_WRITE   │
│     ↑            ↑             │         │
│     └────────────┴─────────────┘         │
│         (invalidation)                   │
└─────────────────────────────────────────┘

Transitions:
1. INVALID → READ_ONLY: Read fault, fetch page
2. INVALID → READ_WRITE: Write fault, fetch + invalidate others
3. READ_ONLY → READ_WRITE: Write fault, acquire exclusive
4. * → INVALID: Receive invalidation message
```

**Write Protocol:**
```
Node A wants to write page P (owned by Node B)
    ↓
1. Send PAGE_REQUEST(P, WRITE) to directory/owner
    ↓
2. Directory sends INVALIDATE to all sharers
    ↓
3. Sharers set page to INVALID, send ACK
    ↓
4. Owner sends PAGE_REPLY with data to Node A
    ↓
5. Node A sets permission to READ_WRITE
    ↓
6. Directory updates owner to Node A
```

### 5. Synchronization Primitives (`src/sync/`)

**Responsibilities:**
- Distributed locks for mutual exclusion
- Barriers for synchronization points

**Lock Protocol (Centralized Manager):**
```
Lock Manager (on manager node):
┌─────────────────────┐
│ Lock ID → State     │
│   State: FREE/HELD  │
│   Holder: node_id   │
│   Waiters: queue    │
└─────────────────────┘

Acquire Flow:
Node → LOCK_REQUEST → Manager
         ↓
    Lock available?
         │
    ┌────┴────┐
   YES        NO
    │          │
    ▼          ▼
Grant lock   Queue request
    │          │
    ▼          ▼
Manager → LOCK_GRANT → Node
```

**Barrier Protocol:**
```
Barrier Manager:
┌─────────────────────────┐
│ Barrier ID → Count      │
│   Expected: N           │
│   Arrived: counter      │
│   Waiters: list         │
└─────────────────────────┘

Flow:
Each node → BARRIER_ARRIVE → Manager
                ↓
         Count++, reached N?
                │
           ┌────┴────┐
          YES        NO
           │          │
           ▼          ▼
    Broadcast      Wait for more
   BARRIER_RELEASE
```

## Page Fault Handling Details

### User-Space Fault Trapping

**Why SIGSEGV?**
- Only user-space mechanism to intercept page faults
- Kernel sends `SIGSEGV` on invalid memory access
- Signal handler can determine fault type and address

**Implementation:**
```c
struct sigaction sa;
sa.sa_flags = SA_SIGINFO;
sa.sa_sigaction = dsm_fault_handler;
sigemptyset(&sa.sa_mask);
sigaction(SIGSEGV, &sa, NULL);
```

**Fault Handler Pseudocode:**
```c
void dsm_fault_handler(int sig, siginfo_t *info, void *context) {
    void *fault_addr = info->si_addr;

    page_entry_t *page = page_table_lookup(fault_addr);
    if (!page) {
        // Not a DSM page, real segfault
        abort();
    }

    access_type_t access = determine_access_type(context);

    if (access == ACCESS_READ) {
        handle_read_fault(page);
    } else {
        handle_write_fault(page);
    }
}
```

## Network Communication Details

### Connection Management

**Manager-Worker Architecture:**
- One manager node coordinates the cluster
- Workers connect to manager at startup
- Peer-to-peer connections for page transfers

**Connection Setup:**
```
Manager:
    socket() → bind() → listen() → accept() [loop]

Worker:
    socket() → connect(manager)

    For each peer:
        socket() → connect(peer)
```

### Message Handling Thread

**Dispatcher Pattern:**
```c
void* message_dispatcher(void *arg) {
    while (running) {
        // Wait for messages on all sockets
        select()/poll() on all connections

        // Read message header
        recv(header)

        // Read payload based on type
        recv(payload)

        // Dispatch to handler
        switch (msg.type) {
            case MSG_PAGE_REQUEST:
                handle_page_request(&msg);
                break;
            case MSG_INVALIDATE:
                handle_invalidate(&msg);
                break;
            // ...
        }
    }
}
```

## Concurrency Considerations

### Thread Safety

**Protected Resources:**
- Page table: `pthread_mutex_t` per table
- Message queue: `pthread_mutex_t` + `pthread_cond_t`
- Lock manager: `pthread_mutex_t` per lock
- Statistics: atomic operations or mutex

**Deadlock Prevention:**
- Always acquire locks in same order (page_id ascending)
- Timeout all blocking operations
- No nested lock acquisitions

### Request Queuing

**Why Queue?**
- Multiple threads may fault on same page simultaneously
- Avoid redundant page transfers

**Implementation:**
```c
page_entry_t {
    bool request_pending;
    pthread_cond_t ready_cv;
}

Thread 1: Page fault → Set pending → Send request → Wait on CV
Thread 2: Page fault → See pending → Wait on CV
...
Handler: Receive reply → Signal CV → Wake all waiters
```

## Performance Considerations

### Critical Path Optimization

**Page Fault Latency:**
```
Fault → Signal handler → Table lookup → Network request →
   Wait for reply → Update permission → Return

Target: < 10ms for local network
```

**Optimizations:**
- Fast page table lookup (hash table for large systems)
- Batched invalidations
- Prefetch adjacent pages
- Caching of page locations

### False Sharing

**Problem:**
```
Node A writes array[0]
Node B writes array[1]
Both on same page → ping-pong effect
```

**Mitigation:**
- Align data structures to page boundaries
- Use padding between thread-local data
- Application-level partitioning

## Failure Handling

**Current Implementation:**
- Timeouts on all network operations
- Retry failed sends (up to N attempts)
- Abort on unrecoverable errors

**Future Enhancements:**
- Node failure detection (heartbeats)
- Page migration on failure
- Checkpoint/recovery

## Design Decisions Rationale

### Single-Writer vs MSI

**Chosen: Single-Writer**
- Simpler to implement correctly
- Easier to reason about consistency
- Fewer message types
- Sufficient for demo purposes

**Trade-off:**
- Less concurrent write performance
- More invalidations than MSI

### Centralized vs Distributed Directory

**Chosen: Centralized (Manager Node)**
- Simple implementation
- Single point of coordination
- Easier debugging

**Trade-off:**
- Manager is bottleneck
- Single point of failure

### TCP vs UDP

**Chosen: TCP**
- Reliability built-in
- Ordered delivery
- Error detection

**Trade-off:**
- Higher latency than UDP
- More overhead

## Code Organization

```
src/
├── core/
│   ├── dsm.c           - Main initialization, API entry points
│   ├── log.c           - Logging infrastructure
│   └── stats.c         - Statistics tracking
├── memory/
│   ├── allocator.c     - dsm_malloc/free implementation
│   ├── page_table.c    - Page table management
│   └── fault_handler.c - SIGSEGV handler
├── network/
│   ├── server.c        - TCP server
│   ├── client.c        - TCP client
│   ├── protocol.c      - Serialization/deserialization
│   └── messenger.c     - Message queue, dispatcher
├── consistency/
│   ├── coherence.c     - Consistency protocol
│   ├── directory.c     - Page ownership directory
│   └── invalidation.c  - Invalidation logic
└── sync/
    ├── lock.c          - Distributed locks
    └── barrier.c       - Barriers
```

## Testing Strategy

### Unit Tests
- Page table operations
- Message serialization/deserialization
- Request queuing
- State transitions

### Integration Tests
- Two-node ping-pong
- Multi-node read sharing
- Concurrent writes
- Lock/barrier correctness

### Stress Tests
- High fault rate
- Random access patterns
- Large memory regions
- Many nodes

---

*This document will be updated as implementation progresses.*

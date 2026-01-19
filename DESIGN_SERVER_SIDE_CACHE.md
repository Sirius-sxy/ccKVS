# Server-Side Cache Architecture Design

## Overview
Redesign ccKVS to move cache from client-side to server-side, with round-robin client request distribution.

## Architecture

### Client Node
- **Role**: Request generator only
- **Components**:
  - Client threads: Generate requests from trace
  - Round-robin scheduler: Distribute requests across all servers
- **No cache**: All requests go to remote servers

### Server Node
- **Role**: Caching and storage layer
- **Components**:
  1. **Cache Layer** (全量热点，对称)
     - Each server has identical cache with hottest keys
     - First lookup point for all incoming requests

  2. **Local KVS Shard** (分片存储)
     - Each server owns a subset of keys (hash-based partitioning)
     - Second lookup point after cache miss

  3. **Forwarding Logic** (新增)
     - If key not in cache and not in local shard
     - Forward to correct server based on key hash
     - Return result to original client

## Request Flow

```
Client                    Server A                     Server B
  │                          │                            │
  │─────Request(key)────────>│                            │
  │                          │                            │
  │                    [1. Check Cache]                   │
  │                          │                            │
  │                     Cache Hit?                        │
  │                          │                            │
  │<─────Response────────────┤ (Yes: Return immediately)  │
  │                          │                            │
  │                     Cache Miss                        │
  │                          │                            │
  │                  [2. Check Local Shard]               │
  │                          │                            │
  │                    In local shard?                    │
  │                          │                            │
  │<─────Response────────────┤ (Yes: Return from KVS)     │
  │                          │                            │
  │                    Not in local shard                 │
  │                          │                            │
  │                  [3. Forward to Owner]                │
  │                          │                            │
  │                          │────Forward(key)───────────>│
  │                          │                            │
  │                          │                      [Check Cache]
  │                          │                            │
  │                          │                      [Check Local KVS]
  │                          │                            │
  │                          │<────Response───────────────┤
  │                          │                            │
  │<─────Response────────────┤                            │
```

## Implementation Status

### Phase 1: Server-Side Cache Integration ✅ COMPLETED
- [x] Add cache to worker (main-server.c, worker.c)
- [x] Implement cache lookup before KVS access (worker-cache.c)
- [x] Handle cache hits in worker
- [x] Separate cache hits from misses

### Phase 2: Key Ownership Detection ✅ COMPLETED
- [x] Implement hash-based key partitioning (key-partition.h)
- [x] Add function: `is_local_key(key, machine_id)`
- [x] Determine key owner: `get_key_owner_machine(key)`
- [x] Separate local and remote keys after cache miss

### Phase 3: Remove Client Cache ✅ COMPLETED
- [x] Remove cache initialization from client (main-client.c)
- [x] Disable KVS initialization on client
- [x] Client becomes pure request generator

### Phase 4: Client Round-Robin ✅ COMPLETED
- [x] Implement round-robin server selection (round-robin.h)
- [x] Replace hash-based routing with RR (cache.c)
- [x] Per-thread RR counters for load balancing

### Phase 5: Server-to-Server Forwarding ✅ COMPLETED
- [x] Create worker-to-worker QPs (using existing remote_wrkr_qp connections)
- [x] Implement request forwarding (worker-forward.c)
- [x] Handle forwarded requests (worker_handle_forwarded_request)
- [x] Return results to original client
- **Implementation**: Uses existing RDMA QPs between workers for forwarding

### Phase 6: Cache Consistency (future work)
- [ ] Handle write invalidations across server caches
- [ ] Implement cache coherence protocol

## Key Design Decisions

### Q1: Cache Content (ANSWERED: 全量)
✅ **Full replication**: Each server caches same hot keys
- Pro: Maximum cache hit rate
- Pro: Simple - no coordination needed
- Con: Memory usage on each server

### Q2: Forwarding Path
**Option A** (Recommended): Server A → Server B → Client
- Server A forwards to Server B
- Server B processes and sends directly to Client
- Pro: Simpler, fewer hops for client
- Con: Server B needs to know client address

**Option B**: Server A → Server B → Server A → Client
- Round-trip through Server A
- Pro: Server B doesn't need client info
- Con: Extra hop, higher latency

### Q3: Cache Placement
✅ **Worker-level cache**: Shared by all workers on same node
- Single cache per server node
- Workers serialize access to cache

## File Changes Required

### Modified Files
1. `src/ccKVS/main-server.c`
   - Enable cache_init for workers
   - Initialize cache with hot keys

2. `src/ccKVS/worker.c`
   - Add cache lookup before KVS access
   - Add forwarding logic for non-local keys
   - Add worker-to-worker communication

3. `src/ccKVS/main-client.c`
   - Remove cache initialization
   - Implement round-robin server selection

4. `src/ccKVS/client-*.c`
   - Remove cache lookup logic
   - Simplify to request generation only

### New Files
1. `include/ccKVS/worker-forward.h` + `src/ccKVS/worker-forward.c`
   - Server-to-server request forwarding implementation
   - Forward requests to key owners
   - Handle forwarded requests and return to original client

2. `include/ccKVS/worker-cache.h` + `src/ccKVS/worker-cache.c`
   - Server-side cache query and management
   - Separate local and remote keys after cache misses

3. `include/ccKVS/key-partition.h`
   - Key ownership determination functions
   - Hash-based key partitioning logic

4. `include/ccKVS/round-robin.h`
   - Round-robin load balancing for clients
   - Distribute requests evenly across servers

## Configuration Changes

### main.h additions:
```c
// Key partitioning
#define KEY_PARTITIONING_ENABLED 1
#define HASH_BASED_PARTITIONING 1

// Server-side cache
#define SERVER_SIDE_CACHE 1

// Forwarding
#define ENABLE_WORKER_FORWARDING 1
#define WORKER_FORWARD_QP_ID 3
```

## Testing Strategy

1. **Single server test**: Verify cache + local KVS works
2. **Two server test**: Verify forwarding works
3. **Multi-server test**: Full deployment with round-robin
4. **Performance test**: Compare with original architecture

## Compatibility

- Keep original client-side cache code as compile-time option
- Use `#ifdef SERVER_SIDE_CACHE` to toggle between architectures
- Maintain backward compatibility with original ccKVS

## Open Questions

1. How to handle cache consistency on writes?
   - Broadcast invalidations to all server caches?
   - Or cache is read-only (writes bypass cache)?

2. Should forwarded requests update Server A's cache?
   - Yes: Learn from forwarding (adaptive cache)
   - No: Keep cache static (simpler)

3. Client response handling:
   - Should client know which server responded?
   - Or transparent forwarding?

## Current Implementation Guide

### Building

```bash
cd src/ccKVS
make separated  # Build both server and client
# Or individually:
make server     # ccKVS-server-sc, ccKVS-server-lin
make client     # ccKVS-client-sc, ccKVS-client-lin
```

### Running

**Server nodes:**
```bash
./run-server.sh
```
This will:
- Initialize server-side cache with hot keys
- Start worker threads
- Wait for client requests

**Client nodes:**
```bash
./run-client.sh
```
This will:
- Generate requests from trace
- Distribute requests round-robin to servers
- No local cache initialization

### Expected Behavior

1. **Cache Hits** (hot keys):
   - Server checks cache first
   - Fast response directly from cache
   - No KVS access needed

2. **Local Shard Hits** (cache miss, local key):
   - Server checks cache → miss
   - Server checks local KVS → hit
   - Response from local KVS

3. **Remote Keys** (cache miss, non-local key):
   - Server checks cache → miss
   - Server detects key belongs to another machine
   - **Currently fails with MICA_RESP_GET_FAIL**
   - TODO: Should forward to correct server

### Known Limitations

⚠️ **Server-to-server forwarding not implemented**
- Requests for keys not in cache AND not in local shard will FAIL
- Need to ensure workload matches key distribution
- Or implement forwarding (Phase 5)

⚠️ **Write consistency not handled**
- Cache may become stale on writes
- Need to implement invalidation/update protocol

⚠️ **Performance considerations**
- Round-robin may cause load imbalance if key distribution is skewed
- Cache should contain most frequently accessed keys
- Local shard only handles ~1/N of total keyspace (N = number of machines)

### Recommended Workload

For best results without forwarding:
1. Ensure cache contains all hot keys (currently 250K keys)
2. Use workload where >90% requests hit cache
3. Or partition workload so each client only accesses keys from one server

### Testing

```bash
# Single server test (all keys local)
MACHINE_NUM=1 make server client

# Multi-server with cache only
# Workload must have high cache hit rate
MACHINE_NUM=3 ./run-server.sh  # on 3 machines
MACHINE_NUM=3 ./run-client.sh  # on client machines
```

## Performance Expectations

### Theoretical Performance

**Cache Hit (best case):**
- Latency: ~1-2 μs (memory access)
- Throughput: Very high (limited by CPU)

**Local KVS (cache miss, local key):**
- Latency: ~5-10 μs (KVS lookup + memory)
- Throughput: Moderate (KVS bottleneck)

**Remote Key (NOT IMPLEMENTED):**
- Would be: ~20-50 μs (RDMA + KVS)
- Currently: FAIL

### Actual vs Original Architecture

**Original (client-side cache):**
- Cache hit: Local memory access (~1 μs)
- Cache miss: RDMA to worker (~10-20 μs)

**New (server-side cache):**
- Cache hit: RDMA + server cache (~10-12 μs)
- Cache miss (local): RDMA + server KVS (~15-25 μs)
- Cache miss (remote): NOT WORKING

**Trade-off**: Slightly higher latency even on cache hits due to network,
but better load distribution and aligns with paper's server-side caching design.

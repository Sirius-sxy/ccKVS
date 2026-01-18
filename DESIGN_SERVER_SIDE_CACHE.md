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

## Implementation Plan

### Phase 1: Server-Side Cache Integration
- [x] Remove cache from client (main-client.c)
- [ ] Add cache to worker (main-server.c, worker.c)
- [ ] Implement cache lookup before KVS access
- [ ] Handle cache hits in worker

### Phase 2: Key Ownership Detection
- [ ] Implement hash-based key partitioning
- [ ] Add function: `is_local_key(key, worker_id)`
- [ ] Determine key owner: `get_key_owner(key)`

### Phase 3: Server-to-Server Forwarding
- [ ] Create worker-to-worker QPs
- [ ] Implement request forwarding
- [ ] Handle forwarded requests
- [ ] Return results to original client

### Phase 4: Client Round-Robin
- [ ] Remove cache logic from client
- [ ] Implement round-robin server selection
- [ ] Simplify client to basic request generator

### Phase 5: Cache Consistency (if writes exist)
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
1. `src/ccKVS/worker-forward.c` (optional)
   - Helper functions for server-to-server forwarding

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

# ccKVS Separated Client-Server Architecture

This document describes the separated client-server architecture for ccKVS, which allows running clients and servers as independent processes.

## Architecture Overview

### Original Architecture (Deprecated)
- Single executable running both client and server threads in the same process
- Executables: `ccKVS-sc`, `ccKVS-lin`

### New Separated Architecture
- **Server executable**: Runs only worker threads
- **Client executable**: Runs only client threads
- Servers and clients can run on different machines
- Better isolation and deployment flexibility

## Build Instructions

### Building All Executables

```bash
cd src/ccKVS
make all
```

This builds:
- `ccKVS-server-sc` - Server with Sequential Consistency protocol
- `ccKVS-server-lin` - Server with Linearizability protocol
- `ccKVS-client-sc` - Client with Sequential Consistency protocol
- `ccKVS-client-lin` - Client with Linearizability protocol

### Building Only Server or Client

```bash
# Build only server executables
make server

# Build only client executables
make client

# Build both server and client (separated mode)
make separated
```

### Cleaning

```bash
# Remove all binaries and object files
make clean

# Remove only object files
make clean-o
```

## Running in Separated Mode

### 1. Start Servers

On each server machine, run:

```bash
cd src/ccKVS
./run-server.sh
```

The script will:
- Auto-detect the machine ID based on IP address
- Initialize RDMA connections
- Start worker threads
- Wait for client connections

**Configuration**: Edit `run-server.sh` to configure:
- `protocol`: Choose "sc" (Sequential Consistency) or "lin" (Linearizability)
- `is_RoCE`: Set to 1 for RoCE, 0 for InfiniBand
- `MEMCACHED_IP`: IP address of the memcached server for QP registry
- `allIPs`: Array of all server IP addresses in the cluster

### 2. Start Clients

Once all servers are running, start clients on client machines:

```bash
cd src/ccKVS
./run-client.sh
```

The script will:
- Auto-detect the machine ID based on IP address
- Connect to remote workers via RDMA
- Start client threads
- Begin issuing requests

**Configuration**: Edit `run-client.sh` with the same settings as server:
- `protocol`: Must match the server protocol
- `is_RoCE`: Must match the server setting
- `MEMCACHED_IP`: Same as server
- `allIPs`: Same array as server

### 3. Manual Execution

You can also run the executables manually:

```bash
# Run server
sudo LD_LIBRARY_PATH=/usr/local/lib/ -E \
    ./ccKVS-server-sc \
    --machine-id 0 \
    --is-roce 0

# Run client
sudo LD_LIBRARY_PATH=/usr/local/lib/ -E \
    ./ccKVS-client-sc \
    --machine-id 1 \
    --is-roce 0
```

## Deployment Scenarios

### Scenario 1: Dedicated Server Machines
- Deploy `ccKVS-server-*` on server machines
- Deploy `ccKVS-client-*` on separate client machines
- Advantages: Better isolation, easier scaling

### Scenario 2: Co-located Clients and Servers
- Run both server and client on the same machines
- Use different machine IDs for client and server processes
- Advantages: Can still test and develop on single machine

### Scenario 3: Hybrid Deployment
- Some machines run only servers
- Some machines run only clients
- Some machines run both
- Advantages: Maximum flexibility

## Migration from Original Architecture

If you were using the original unified executables (`ccKVS-sc` or `ccKVS-lin`):

1. **No code changes required** - The original executables are still built for backward compatibility
2. **To migrate to separated mode**:
   - Replace `./run-ccKVS.sh` with `./run-server.sh` on server machines
   - Run `./run-client.sh` on client machines
   - Ensure both use the same protocol (sc or lin)

## Troubleshooting

### Clients can't connect to servers
- Ensure servers are started before clients
- Verify MEMCACHED_IP is accessible from all machines
- Check that allIPs array matches across server and client scripts
- Ensure RDMA network is properly configured

### Process fails to start
- Check hugepages are configured: `cat /sys/devices/system/node/node*/hugepages/hugepages-2048kB/nr_hugepages`
- Verify shared memory settings: `cat /proc/sys/kernel/shmmax`
- Clean up stale processes: `sudo killall ccKVS-server-sc ccKVS-client-sc`
- Remove stale SHM keys: See cleanup commands in run-*.sh scripts

### Protocol mismatch
- Ensure clients and servers use the same protocol (sc or lin)
- Verify the `protocol` variable is set correctly in both scripts

## Architecture Details

### Server (main-server.c)
- Initializes worker threads only
- Sets up RDMA queue pairs for receiving client requests
- Handles KVS operations (GET/PUT)
- Manages coherence protocol (invalidations/updates)

### Client (main-client.c)
- Initializes client threads only
- Sets up RDMA queue pairs for sending requests
- Manages local cache
- Handles coherence messages (invalidations/updates)
- Issues workload requests to workers

### Shared Components
- `worker.c`: Worker thread logic (server-side)
- `client-sc.c` / `client-lin.c`: Client thread logic
- `cache.c`: Cache management
- `util.c`: Utility functions
- `stats.c`: Statistics collection

## Performance Considerations

- **Network latency**: Separated mode adds network hop between client and server
- **RDMA optimization**: Use RoCE or InfiniBand for best performance
- **Thread pinning**: Scripts automatically pin threads to CPU cores
- **Memory**: Hugepages are required for both client and server

## Future Enhancements

- Support for dynamic server discovery
- Load balancing across multiple servers
- Fault tolerance and failover
- Containerized deployment

# ccKVS Server-Side Cache Deployment Guide

完整的编译、配置和运行指南，用于部署server-side caching架构。

---

## 1. 编译步骤

### 1.1 清理旧编译文件
```bash
cd /home/user/ccKVS/src/ccKVS
make clean
```

### 1.2 编译所有组件
```bash
# 编译所有必要的库和可执行文件
cd /home/user/ccKVS/src/libhrd
make

cd /home/user/ccKVS/src/mica
make

cd /home/user/ccKVS/src/ccKVS
make separated   # 同时编译server和client

# 或者分别编译
make server      # 只编译 ccKVS-server-sc 和 ccKVS-server-lin
make client      # 只编译 ccKVS-client-sc 和 ccKVS-client-lin
```

### 1.3 验证编译结果
```bash
ls -lh ccKVS-server-* ccKVS-client-*

# 应该看到以下可执行文件:
# ccKVS-server-sc   (Sequential Consistency server)
# ccKVS-server-lin  (Linearizability server)
# ccKVS-client-sc   (Sequential Consistency client)
# ccKVS-client-lin  (Linearizability client)
```

---

## 2. 配置修改

### 2.1 核心配置文件：`include/ccKVS/main.h`

**必须修改的参数：**

```c
// ========== 集群拓扑配置 ==========
#define WORKERS_PER_MACHINE 10          // 每个server的worker线程数
#define CLIENTS_PER_MACHINE 10          // 每个client机器的client线程数
#define MACHINE_NUM 3                   // 集群中的机器总数 (修改为你的机器数量)

// ========== 硬件配置 ==========
#define TOTAL_CORES 40                  // 机器的总CPU核数
#define SOCKET_NUM 2                    // CPU socket数量
#define PHYSICAL_CORES_PER_SOCKET 10    // 每个socket的物理核数
#define VIRTUAL_CORES_PER_SOCKET 20     // 每个socket的虚拟核数(含超线程)
```

**示例配置（根据你的拓扑修改）：**

```c
// 示例1: 3台server + 2台client (每台机器20核)
#define MACHINE_NUM 3                   // 3台servers参与KVS
#define WORKERS_PER_MACHINE 10          // 每台server 10个workers
#define CLIENTS_PER_MACHINE 8           // 每台client 8个client线程
#define TOTAL_CORES 20
#define PHYSICAL_CORES_PER_SOCKET 10

// 示例2: 5台server + 3台client (每台机器40核)
#define MACHINE_NUM 5                   // 5台servers
#define WORKERS_PER_MACHINE 16          // 每台server 16个workers
#define CLIENTS_PER_MACHINE 16          // 每台client 16个client线程
#define TOTAL_CORES 40
#define PHYSICAL_CORES_PER_SOCKET 10
```

### 2.2 数据规模配置：`include/libhrd/hrd.h`

```c
// ========== KVS数据规模 ==========
#define USE_BIG_OBJECTS 0               // 0=小对象(32B), 1=大对象
#define BASE_VALUE_SIZE 32              // Value大小(字节)

// 总KVS数据量
#define HERD_NUM_KEYS (4 * 1024 * 1024)    // 4M keys (约192MB总数据)
#define HERD_NUM_BKTS (2 * 1024 * 1024)    // 2M buckets

// ========== Cache配置 ==========
// 在 include/ccKVS/cache.h 中:
#define CACHE_NUM_KEYS (250 * 1000)        // 250K hot keys (约12MB cache)
#define CACHE_NUM_BKTS (64 * 1024)         // 64K buckets
```

**数据量调整示例：**

```c
// 小规模测试 (适合开发环境)
#define HERD_NUM_KEYS (1 * 1024 * 1024)    // 1M keys (~48MB)
#define CACHE_NUM_KEYS (100 * 1000)        // 100K cache (~5MB)

// 中等规模
#define HERD_NUM_KEYS (4 * 1024 * 1024)    // 4M keys (~192MB) - 默认
#define CACHE_NUM_KEYS (250 * 1000)        // 250K cache (~12MB) - 默认

// 大规模
#define HERD_NUM_KEYS (8 * 1024 * 1024)    // 8M keys (~384MB)
#define CACHE_NUM_KEYS (500 * 1000)        // 500K cache (~24MB)
```

### 2.3 网络配置：`run-server.sh` 和 `run-client.sh`

**在 run-server.sh 中修改（第8行和第14行）：**

```bash
# Memcached服务器IP (用于RDMA QP初始化)
export MEMCACHED_IP="192.168.1.100"    # 修改为你的memcached节点IP

# 集群中所有机器的IP列表
allIPs=(192.168.1.100 192.168.1.101 192.168.1.102)  # 修改为你的server IPs

# 注意: allIPs数组长度必须 >= MACHINE_NUM
```

**在 run-client.sh 中同样修改（第8行和第14行）：**

```bash
export MEMCACHED_IP="192.168.1.100"    # 与server保持一致
allIPs=(192.168.1.100 192.168.1.101 192.168.1.102)  # 与server保持一致
```

### 2.4 协议选择

**在 run-server.sh 和 run-client.sh 中（第6行）：**

```bash
# 选择一致性协议
protocol="sc"   # Sequential Consistency (推荐，已实现coherence)
# protocol="lin"  # Linearizability (需要额外的invalidation逻辑)
```

**注意**:
- **SC (Sequential Consistency)**: 使用broadcast updates，Phase 6已完整实现
- **LIN (Linearizability)**: 需要broadcast invalidations + ACKs，更严格但开销更大

---

## 3. 运行步骤

### 3.1 前置准备（所有节点）

**安装依赖：**
```bash
cd /home/user/ccKVS
sudo bash bin/install-dependences.sh

# 主要依赖:
# - RDMA drivers (MLNX_OFED)
# - libmemcached
# - libnuma
# - GSL (GNU Scientific Library)
```

**配置Hugepages（所有节点）：**
```bash
# 查看当前hugepage配置
cat /proc/meminfo | grep Huge

# 设置hugepages (每个server节点至少需要2GB)
echo 1024 | sudo tee /proc/sys/vm/nr_hugepages  # 1024 * 2MB = 2GB

# 验证
cat /proc/meminfo | grep HugePages_Free
```

**启动Memcached（只在一个节点）：**
```bash
# 在第一台server上启动 (IP对应MEMCACHED_IP)
sudo killall memcached
memcached -l 0.0.0.0 -p 11211 &

# 验证
telnet 192.168.1.100 11211
```

### 3.2 启动Server节点

**在每台Server机器上（按顺序）：**

```bash
cd /home/user/ccKVS/src/ccKVS

# Server 0 (IP: 192.168.1.100)
sudo bash run-server.sh
# 输出: Machine-Id 0
# 等待: "Worker threads initialized"

# Server 1 (IP: 192.168.1.101)
sudo bash run-server.sh
# 输出: Machine-Id 1

# Server 2 (IP: 192.168.1.102)
sudo bash run-server.sh
# 输出: Machine-Id 2
```

**启动成功标志：**
```
Machine-Id 0
SERVER MODE: Starting ccKVS server
Worker 0: Cache coherence enabled
Worker 1: Cache coherence enabled
...
Workers ready, waiting for clients
```

### 3.3 启动Client节点

**在Client机器上（等待所有servers启动后）：**

```bash
cd /home/user/ccKVS/src/ccKVS

# Client节点可以与server共机器，也可以单独机器
sudo bash run-client.sh
# 输出: Machine-Id X
# 开始发送请求
```

**运行成功输出：**
```
CLIENT MODE: Starting ccKVS client
Machine-Id 0
Client 0: Round-robin distribution enabled
Client 0: Sending requests to all servers...
Throughput: 2.5M ops/s
Cache hit rate: 94.3%
```

---

## 4. 配置检查清单

### 4.1 编译前检查

- [ ] `main.h`: `MACHINE_NUM` = 实际server数量
- [ ] `main.h`: `WORKERS_PER_MACHINE` 和 `CLIENTS_PER_MACHINE` 设置合理
- [ ] `main.h`: `TOTAL_CORES` 匹配机器硬件
- [ ] `hrd.h`: `HERD_NUM_KEYS` 和 `CACHE_NUM_KEYS` 根据内存调整
- [ ] 所有修改后执行 `make clean && make separated`

### 4.2 运行前检查

- [ ] `run-server.sh`: `MEMCACHED_IP` 设置正确
- [ ] `run-server.sh`: `allIPs` 包含所有server IPs，顺序正确
- [ ] `run-client.sh`: `MEMCACHED_IP` 和 `allIPs` 与server一致
- [ ] `run-server.sh` 和 `run-client.sh`: `protocol` 选择一致（都用"sc"或"lin"）
- [ ] Hugepages已配置: `cat /proc/meminfo | grep HugePages_Free` > 0
- [ ] Memcached已在第一台server上启动
- [ ] RDMA网卡已正确配置: `ibv_devices` 能看到设备

### 4.3 网络拓扑检查

**确认机器ID分配：**
```
Server 0: IP在allIPs[0] → machine_id=0
Server 1: IP在allIPs[1] → machine_id=1
Server 2: IP在allIPs[2] → machine_id=2
...
```

**RDMA连通性测试：**
```bash
# 在Server 0上
ibv_rc_pingpong -d mlx5_0 -g 0

# 在Server 1上
ibv_rc_pingpong -d mlx5_0 -g 0 <Server 0 IP>
```

---

## 5. 故障排查

### 5.1 编译错误

**错误: "undefined reference to worker_coherence_init"**
```bash
# 检查Makefile中是否包含worker-coherence.o
grep worker-coherence src/ccKVS/Makefile

# 应该在ccKVS-server-sc和ccKVS-server-lin行中看到worker-coherence.o
```

**错误: "hrd.h: No such file or directory"**
```bash
# 先编译依赖库
cd src/libhrd && make
cd src/mica && make
```

### 5.2 运行时错误

**错误: "HRD: Couldn't find device mlx5_0"**
```bash
# 检查RDMA设备
ibv_devices

# 如果没有设备，重新安装OFED驱动
sudo /etc/init.d/openibd restart
```

**错误: "Failed to allocate hugepages"**
```bash
# 增加hugepage数量
echo 2048 | sudo tee /proc/sys/vm/nr_hugepages

# 清理旧的hugepages
sudo bash bin/clean-pages.sh
```

**错误: "Connection timeout to memcached"**
```bash
# 检查memcached是否运行
ps aux | grep memcached

# 检查防火墙
sudo iptables -I INPUT -p tcp --dport 11211 -j ACCEPT

# 测试连接
telnet $MEMCACHED_IP 11211
```

**错误: "Worker coherence init failed"**
```bash
# 检查remote_wrkr_qp是否正确初始化
# 确保所有servers都已启动
# 检查worker.c中ENABLE_CACHE_COHERENCE是否为1
```

### 5.3 性能问题

**Cache hit rate很低:**
- 检查`CACHE_NUM_KEYS`是否足够大
- 确认cache预加载成功: `grep "Populated" server_log`
- 验证workload是否有热点访问模式

**Throughput很低:**
- 增加`WORKERS_PER_MACHINE`和`CLIENTS_PER_MACHINE`
- 检查CPU核心是否被充分利用: `htop`
- 验证RDMA网卡是否工作在最佳模式: `ibstat`

---

## 6. 推荐配置模板

### 6.1 小规模测试 (3台server, 开发环境)

**main.h:**
```c
#define MACHINE_NUM 3
#define WORKERS_PER_MACHINE 4
#define CLIENTS_PER_MACHINE 4
#define TOTAL_CORES 20
```

**hrd.h:**
```c
#define HERD_NUM_KEYS (1 * 1024 * 1024)  // 1M keys
```

**cache.h:**
```c
#define CACHE_NUM_KEYS (100 * 1000)      // 100K cache
```

### 6.2 中等规模 (5台server, 生产测试)

**main.h:**
```c
#define MACHINE_NUM 5
#define WORKERS_PER_MACHINE 10
#define CLIENTS_PER_MACHINE 10
#define TOTAL_CORES 40
```

**hrd.h:**
```c
#define HERD_NUM_KEYS (4 * 1024 * 1024)  // 4M keys (默认)
```

**cache.h:**
```c
#define CACHE_NUM_KEYS (250 * 1000)      // 250K cache (默认)
```

### 6.3 大规模 (10台server, 高性能)

**main.h:**
```c
#define MACHINE_NUM 10
#define WORKERS_PER_MACHINE 16
#define CLIENTS_PER_MACHINE 16
#define TOTAL_CORES 40
```

**hrd.h:**
```c
#define HERD_NUM_KEYS (8 * 1024 * 1024)  // 8M keys
```

**cache.h:**
```c
#define CACHE_NUM_KEYS (500 * 1000)      // 500K cache
```

---

## 7. 验证部署

### 7.1 检查Server状态

```bash
# Server日志中应该看到:
# - "Worker X: Cache coherence enabled"
# - "Coherence context initialized"
# - "Workers ready"

# 检查processes
ps aux | grep ccKVS-server
```

### 7.2 检查Client输出

```bash
# Client应该输出统计信息:
# - Throughput (ops/s)
# - Cache hit rate
# - Latency分布

# 正常输出示例:
# Throughput: 2.5M ops/s
# Cache hit rate: 94.3%
# Avg latency: 12 us
```

### 7.3 监控系统资源

```bash
# CPU使用率
htop

# 网络流量
iftop -i <RDMA_interface>

# 内存使用
free -h
cat /proc/meminfo | grep Huge
```

---

## 8. 快速启动命令总结

```bash
# === 1. 编译 (一次) ===
cd /home/user/ccKVS/src/ccKVS
make clean
make separated

# === 2. 配置 (修改一次后不需要每次改) ===
# 修改 include/ccKVS/main.h 中的 MACHINE_NUM
# 修改 run-server.sh 和 run-client.sh 中的 IPs

# === 3. 启动Memcached (Server 0) ===
memcached -l 0.0.0.0 &

# === 4. 启动Servers (每台server机器) ===
cd /home/user/ccKVS/src/ccKVS
sudo bash run-server.sh

# === 5. 启动Clients (等所有servers就绪后) ===
cd /home/user/ccKVS/src/ccKVS
sudo bash run-client.sh

# === 6. 停止所有 ===
sudo killall ccKVS-server-sc ccKVS-client-sc
sudo killall memcached
```

---

## 附录: Phase 6特性验证

### 验证Cache Coherence工作

1. **启动2台servers + 1台client**
2. **在Client上运行写操作**
3. **检查Server日志**:
   ```
   Worker 0: Broadcasting update for key XXX
   Worker 1: Received coherence update from Server 0
   Worker 1: Applied update to local cache
   ```
4. **验证一致性**: 所有servers应该看到相同的cache内容

---

如有问题，请参考：
- `/home/user/ccKVS/DESIGN_SERVER_SIDE_CACHE.md` - 架构设计文档
- `/home/user/ccKVS/README.md` - 原始ccKVS文档
- GitHub Issues: https://github.com/icsa-caps/ccKVS/issues

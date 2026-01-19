# 编译问题修复说明

## ✅ 已修复的问题

### 1. Multiple Definition Linker Errors（多重定义链接错误）

**错误信息：**
```
/usr/bin/ld: hrd_util.o:(.bss+0x10): multiple definition of `remote_IP'
/usr/bin/ld: hrd_util.o:(.bss+0x0): multiple definition of `remote_id'
/usr/bin/ld: hrd_util.o:(.bss+0x8): multiple definition of `local_IP'
...
```

**问题原因：**
- 在 `include/libhrd/hrd.h` 中，全局变量被**定义**而非声明
- 每个包含该头文件的 `.c` 文件都创建了变量的副本
- 链接时出现多重定义冲突

**修复方案：**
- 在 `hrd.h` 中将定义改为 `extern` 声明：
  ```c
  // Before:
  int is_roce, is_master, is_client;

  // After:
  extern int is_roce, is_master, is_client;
  ```

- 在 `src/libhrd/hrd_conn.c` 中添加真正的定义：
  ```c
  // Global variable definitions (declared as extern in hrd.h)
  int is_roce, is_master, is_client;
  int machine_id, machines_num;
  char *remote_IP, *local_IP;
  int remote_id;
  ```

**状态**: ✅ 已修复并提交（Commit 907e4ca）

---

### 2. Type Conflict for machine_id（类型冲突错误）

**错误信息：**
```
worker-cache.c:13:16: error: conflicting types for 'machine_id'; have 'uint8_t'
../../include/libhrd/hrd.h:161:12: note: previous declaration of 'machine_id' with type 'int'
```

**问题原因：**
- `hrd.h` 中 `machine_id` 被声明为 `int`
- `worker-cache.c`, `worker-coherence.c`, `worker-forward.c` 中错误地重新声明为 `uint8_t`
- 类型冲突导致编译失败

**修复方案：**
- 删除worker模块中的重复 `extern` 声明
- 所有模块统一使用 `hrd.h` 中的 `int` 类型声明
- 添加注释说明 `machine_id` 已在 `hrd.h` 中声明

**修改的文件：**
- `src/ccKVS/worker-cache.c`
- `src/ccKVS/worker-coherence.c`
- `src/ccKVS/worker-forward.c`

**状态**: ✅ 已修复并提交（Commit 3a0feed）

---

### 3. 编译器版本问题

**错误信息：**
```
make: gcc-6: No such file or directory
```

**问题原因：**
- Makefiles硬编码使用 `gcc-6`
- 现代系统通常只有 `gcc` 或更新版本

**修复方案：**
- 更新所有Makefile：`gcc-6` → `gcc`
- 修改的文件：
  - `src/libhrd/Makefile`
  - `src/mica/Makefile`
  - `src/ccKVS/Makefile`

**状态**: ✅ 已修复并提交（Commit 907e4ca）

---

### 4. Header Inclusion Order Issues（头文件包含顺序问题）

**错误信息（错误1）：**
```
../../include/optik/utils.h:711:22: error: 'CORES_PER_SOCKET' undeclared
../../include/optik/utils.h:742:23: error: 'the_cores' undeclared
```

**问题原因：**
- `optik/utils.h` 需要 `DEFAULT` 和 `CORE_NUM` 宏定义
- 这些宏在 `cache.h` 中定义
- `worker-forward.h` 直接包含 `optik_mod.h` 而没有先包含 `cache.h`
- 导致 `utils.h` 在没有这些定义的情况下被编译

**修复方案：**
- 在 `worker-forward.h` 中用 `cache.h` 替换单独的头文件包含
- `cache.h` 会按正确顺序包含所有需要的头文件
  ```c
  // Before:
  #include "mica.h"
  #include "hrd.h"
  #include "optik_mod.h"
  #include "main.h"

  // After:
  #include "cache.h"  // Defines DEFAULT and CORE_NUM, includes all needed headers
  ```

**错误信息（错误2）：**
```
../../include/optik/optik_mod.h:44:10: fatal error: atomic_ops.h: No such file or directory
```

**问题原因：**
- `optik_mod.h` 使用 `<atomic_ops.h>`（系统头文件）
- 实际文件名是 `atomic_ops_if.h`（本地头文件）
- 应该使用 `"atomic_ops_if.h"` 而不是 `<atomic_ops.h>`

**修复方案：**
- 修改 `include/optik/optik_mod.h`：
  ```c
  // Before:
  #include <atomic_ops.h>

  // After:
  #include "atomic_ops_if.h"
  ```

**错误信息（错误3 - 警告）：**
```
../../include/optik/utils.h:744:9: warning: implicit declaration of function 'CPU_ZERO'
../../include/optik/utils.h:745:9: warning: implicit declaration of function 'CPU_SET'
../../include/optik/utils.h:750:13: warning: implicit declaration of function 'pthread_setaffinity_np'
```

**问题原因：**
- `CPU_ZERO`, `CPU_SET`, `pthread_setaffinity_np` 需要 `_GNU_SOURCE` 宏
- `utils.h` 在包含系统头文件（如 `<sched.h>`）之前没有定义 `_GNU_SOURCE`
- 虽然 `optik_mod.h` 定义了它，但 `utils.h` 自己的包含发生得更早

**修复方案：**
- 在 `include/optik/utils.h` 顶部添加 `_GNU_SOURCE` 定义：
  ```c
  #ifndef _UTILS_H_INCLUDED_
  #define _UTILS_H_INCLUDED_

  #ifndef _GNU_SOURCE
  #define _GNU_SOURCE
  #endif

  #include <stdlib.h>
  // ... other includes
  ```

**修改的文件：**
- `include/ccKVS/worker-forward.h` - 修改头文件包含顺序
- `include/optik/optik_mod.h` - 修复 atomic_ops.h 路径
- `include/optik/utils.h` - 添加 _GNU_SOURCE 定义

**状态**: ✅ 已修复（Commit 1d7378f）

**注**: CPU_ZERO/CPU_SET警告在某些系统上可能依然存在，但这是无害的警告，不影响功能

---

### 5. Type Conflict for remote_wrkr_qp（remote_wrkr_qp类型冲突）

**错误信息：**
```
worker-coherence.c:9:27: error: conflicting types for 'remote_wrkr_qp'; have 'struct remote_qp **'
../../include/ccKVS/main.h:343:25: note: previous declaration of 'remote_wrkr_qp' with type 'struct remote_qp[1][30]'
```

**问题原因：**
- `worker-coherence.c` 中错误地将 `remote_wrkr_qp` 声明为双指针 `struct remote_qp **`
- 实际类型应该是二维数组 `struct remote_qp[WORKER_NUM_UD_QPS][WORKER_NUM]`
- 与 `main.h` 和 `worker-forward.c` 中的声明不一致

**修复方案：**
- 修改 `worker-coherence.c` 中的声明以匹配 `main.h`：
  ```c
  // Before:
  extern struct remote_qp **remote_wrkr_qp;

  // After:
  extern struct remote_qp remote_wrkr_qp[WORKER_NUM_UD_QPS][WORKER_NUM];
  ```

**修改的文件：**
- `src/ccKVS/worker-coherence.c` - 修正 remote_wrkr_qp 类型声明

**状态**: ✅ 已修复（新提交）

---

## ⚠️ 需要手动处理的依赖

### 必需的系统依赖

由于网络问题无法自动安装，**需要在实际部署环境中手动安装**：

```bash
# Ubuntu/Debian
sudo apt-get install -y \
    libnuma-dev \
    libibverbs-dev \
    librdmacm-dev \
    libmemcached-dev \
    libgsl-dev \
    rdma-core

# CentOS/RHEL
sudo yum install -y \
    numactl-devel \
    libibverbs-devel \
    librdmacm-devel \
    libmemcached-devel \
    gsl-devel \
    rdma-core
```

### RDMA驱动（MLNX_OFED）

对于生产环境，需要安装Mellanox OFED驱动：

```bash
# 下载MLNX_OFED
wget https://www.mellanox.com/downloads/ofed/MLNX_OFED-<version>/MLNX_OFED_LINUX-<version>-ubuntu<version>-x86_64.tgz

# 解压并安装
tar xzf MLNX_OFED_LINUX-*.tgz
cd MLNX_OFED_LINUX-*/
sudo ./mlnxofedinstall --all

# 重启RDMA服务
sudo /etc/init.d/openibd restart
```

---

## 📋 编译步骤总结

### 1. 确认依赖已安装

```bash
# 检查关键库
dpkg -l | grep -E "libnuma|libibverbs|librdmacm|libmemcached|libgsl"

# 检查RDMA设备
ibv_devices
```

### 2. 编译项目

```bash
cd /home/user/ccKVS

# 清理旧编译产物
cd src/libhrd && make clean
cd ../mica && make clean
cd ../ccKVS && make clean

# 编译依赖库
cd /home/user/ccKVS/src/libhrd
make

cd /home/user/ccKVS/src/mica
make

# 编译主程序
cd /home/user/ccKVS/src/ccKVS
make separated  # 编译server和client

# 或者分别编译
make server     # 只编译server
make client     # 只编译client
```

### 3. 验证编译结果

```bash
cd /home/user/ccKVS/src/ccKVS
ls -lh ccKVS-*

# 应该看到:
# ccKVS-server-sc
# ccKVS-server-lin
# ccKVS-client-sc
# ccKVS-client-lin
```

---

## 🔧 可能的编译错误及解决方案

### 错误：`conflicting types for 'machine_id'`

**错误信息：**
```
error: conflicting types for 'machine_id'; have 'uint8_t'
note: previous declaration of 'machine_id' with type 'int'
```

**解决**: ✅ 已在 Commit 3a0feed 中修复
- 问题是 worker 模块中重复声明了 `machine_id` 为错误类型
- 如果你从旧版本升级，请拉取最新代码

### 错误：`fatal error: numaif.h: No such file or directory`

**解决**: 安装 `libnuma-dev`
```bash
sudo apt-get install libnuma-dev
```

### 错误：`fatal error: infiniband/verbs.h: No such file or directory`

**解决**: 安装 `libibverbs-dev`
```bash
sudo apt-get install libibverbs-dev rdma-core
```

### 错误：`fatal error: libmemcached/memcached.h: No such file or directory`

**解决**: 安装 `libmemcached-dev`
```bash
sudo apt-get install libmemcached-dev
```

### 错误：`undefined reference to gsl_xxx`

**解决**: 安装 `libgsl-dev`
```bash
sudo apt-get install libgsl-dev
```

### 错误：链接时 `cannot find -lrdmacm`

**解决**: 安装 `librdmacm-dev`
```bash
sudo apt-get install librdmacm-dev
```

---

## 🚀 快速编译（假设依赖已安装）

```bash
#!/bin/bash
# quick-compile.sh

set -e  # Exit on error

cd /home/user/ccKVS

# Clean
echo "Cleaning old build artifacts..."
find src -name "*.o" -delete
rm -f src/ccKVS/ccKVS-*

# Build libraries
echo "Building libhrd..."
cd src/libhrd && make

echo "Building mica..."
cd ../mica && make

# Build main executables
echo "Building ccKVS server and client..."
cd ../ccKVS && make separated

echo ""
echo "✅ Compilation successful!"
echo "Executables:"
ls -lh ccKVS-server-* ccKVS-client-*
```

保存为 `quick-compile.sh` 并运行：
```bash
chmod +x quick-compile.sh
./quick-compile.sh
```

---

## 📝 关键修改记录

| Commit | 修改内容 | 文件 |
|--------|----------|------|
| 907e4ca | 修复多重定义错误 | `hrd.h`, `hrd_conn.c` |
| 907e4ca | 更新编译器版本 | All `Makefile`s |
| 3a0feed | 修复machine_id类型冲突 | `worker-cache.c`, `worker-coherence.c`, `worker-forward.c` |

---

## 💡 注意事项

1. **环境差异**:
   - 开发环境可能没有RDMA硬件，编译可能通过但无法运行
   - 生产环境必须有InfiniBand/RoCE网卡

2. **网络隔离环境**:
   - 如果无法访问互联网，需要离线下载所有依赖包
   - 使用 `apt-get download` 下载 `.deb` 包后传输到目标机器

3. **Docker环境**:
   - RDMA需要特殊的Docker配置 (`--device` 参数)
   - 推荐直接在物理机或VM上运行

4. **交叉编译**:
   - 如果在x86编译ARM版本，需要安装交叉编译工具链
   - 修改Makefile中的 `CC` 为对应的交叉编译器

---

## 📚 参考文档

- 完整部署指南: `/home/user/ccKVS/DEPLOYMENT_GUIDE.md`
- 架构设计文档: `/home/user/ccKVS/DESIGN_SERVER_SIDE_CACHE.md`
- 原始ccKVS README: `/home/user/ccKVS/README.md`
- RDMA编程指南: https://github.com/efficient/rdma_bench

---

## ✉️ 获取帮助

如果遇到编译问题：

1. 检查依赖是否完整安装
2. 查看编译错误日志
3. 参考本文档的"可能的编译错误"部分
4. 检查RDMA硬件是否正确配置 (`ibv_devices`)

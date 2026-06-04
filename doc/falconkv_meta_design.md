# FalconKV Meta 模块设计文档

## 1. 模块概述

Meta 模块是 FalconKV 的全局元数据聚合/查询服务，基于纯内存实现，负责：

- **元数据聚合**：接收 Store 推送的元数据（SyncCommit/SyncRemove），维护全局 key 索引
- **元数据查询**：响应 Client 的 BatchExist/BatchLookup 请求，提供 key 描述信息
- **Store 注册**：管理 Store 节点的注册与心跳

### 1.1 核心约束

| 约束项 | 说明 |
|--------|------|
| 存储引擎 | 纯内存（分片哈希表 + 分片读写锁） |
| 并发策略 | 分片级读写锁（读共享、写排他） |
| 分片策略 | 基于 key hash 分片到 64 个独立哈希表 |
| 部署模式 | 独立进程 `falconkv_master`，通过 brpc 对外提供服务 |
| 容错模式 | Meta 不可用时 Store/Client 仍可正常运行（本地模式），自动重连后全量重新同步 |

### 1.2 设计原则

Meta 作为全局元数据聚合服务，**不负责空间分配和驱逐决策**。空间管理（SlotAllocator）和驱逐策略由各 Store 节点本地执行。Meta 通过接收 Store 推送的 SyncCommit/SyncRemove 消息来维护全局元数据视图。

## 2. 整体架构

### 2.1 模块架构图

```
┌──────────────────────────────────────────────────────────────────┐
│                    FalconKV Meta Service                          │
│                                                                   │
│  ┌───────────────────────────────────────────────────────────┐   │
│  │ RPC Service Layer (brpc, multi-threaded)                   │   │
│  │  - BatchExist RPC        (Client 查询 key 存在性)          │   │
│  │  - BatchLookup RPC       (Client 查询 key 描述)           │   │
│  │  - SyncCommit RPC        (Store 推送已提交 key)            │   │
│  │  - SyncRemove RPC        (Store 推送已删除/驱逐 key)       │   │
│  │  - StoreRegister RPC     (Store 注册)                     │   │
│  │  - ClientHeartbeat RPC   (Client 心跳)                    │   │
│  └───────────────────────┬───────────────────────────────────┘   │
│                          │                                        │
│  ┌───────────────────────┼────────────────────────────────────┐  │
│  │ Business Logic Layer  │                                    │  │
│  │                       ▼                                    │  │
│  │  ┌──────────────────────────────────────────────────┐      │  │
│  │  │ MetaManager (分片内存存储)                         │      │  │
│  │  │                                                  │      │  │
│  │  │  ┌─────────┐ ┌─────────┐       ┌─────────┐      │      │  │
│  │  │  │ Shard 0 │ │ Shard 1 │  ...  │ Shard N │      │      │  │
│  │  │  │ [rwlock]│ │ [rwlock]│       │ [rwlock]│      │      │  │
│  │  │  │ hashmap │ │ hashmap │       │ hashmap │      │      │  │
│  │  │  └─────────┘ └─────────┘       └─────────┘      │      │  │
│  │  │                                                  │      │  │
│  │  │  ┌────────────────────────────────────────┐      │      │  │
│  │  │  │ stores_ (独立读写锁保护)                  │      │      │  │
│  │  │  │ store_id → StoreInfo                    │      │      │  │
│  │  │  └────────────────────────────────────────┘      │      │  │
│  │  └──────────────────────────────────────────────────┘      │  │
│  └────────────────────────────────────────────────────────────┘  │
└──────────────────────────────────────────────────────────────────┘
```

### 2.2 数据流

```
元数据写入流（Store 推送）:
  Store.BatchPut() → 内部分配空间 + 写入 SSD
  Store → MetaSyncClient.SyncCommit(store_id, records) → Meta 接收并存储

元数据删除流（Store 推送）:
  Store 内部驱逐 → MetaSyncClient.SyncRemove(store_id, keys) → Meta 接收并删除

元数据查询流（Client 请求）:
  Client.BatchExist(keys) → Meta 查询分片哈希表 → 返回命中的 key_descs
  Client.BatchLookup(keys) → Meta 查询分片哈希表 → 返回 key 描述信息
```

## 3. 数据结构设计

### 3.1 KeyRecord

```cpp
struct KeyRecord {
    std::string key;
    uint32_t store_id = 0;
    uint64_t offset = 0;
    uint32_t size = 0;
    int stat = 0;                 // 1 = committed, 2 = evict
    uint64_t access_time_ms = 0;
    uint64_t create_time_ms = 0;
    std::string client_id;
};
```

**状态定义**：

| stat 值 | 状态 | 说明 |
|----------|------|------|
| 1 | committed | 数据写入完成，可正常读取 |
| 2 | evict | 数据已驱逐，空间已回收 |

> **注意**：Meta 不存储 `stat=0 (start)` 状态。Store 在内部完成空间分配和数据写入后，通过 SyncCommit 推送 `stat=1 (committed)` 的记录到 Meta。

### 3.2 StoreInfo

```cpp
struct StoreInfo {
    uint32_t store_id = 0;
    uint32_t node_id = 0;
    std::string store_addr;   // 远程 RPC 路由用
    std::string data_file;    // 同节点 DirectIO 用
    uint32_t chunk_size = 0;  // DirectIO 对齐用
};
```

Meta 仅存储 Store 路由和访问所需的元信息。空间使用率由各 Store 自行管理，Meta 不再追踪。

### 3.3 分片结构

```cpp
struct Shard {
    std::shared_mutex rwlock;
    std::unordered_map<std::string, KeyRecord> key_records;
};
```

- 默认 64 个分片，每个分片独立的 `std::shared_mutex`
- `shard_id = std::hash<std::string>{}(key) % shard_count`
- 不同 key 自然分散到不同分片，降低锁争用

## 4. 并发读写策略

### 4.1 读写锁策略

| 操作 | 锁类型 | 说明 |
|------|--------|------|
| BatchExist | shared_lock（读锁） | 多个读操作可并发执行 |
| BatchLookup | shared_lock（读锁） | 多个读操作可并发执行 |
| SyncCommit | unique_lock（写锁） | 写操作排他，仅锁定涉及的分片 |
| SyncRemove | unique_lock（写锁） | 写操作排他，仅锁定涉及的分片 |
| RegisterStore | unique_lock（写锁） | 锁定独立的 stores_rwlock_ |

### 4.2 批量操作分片分组

所有批量操作（BatchExist、BatchLookup、SyncCommit、SyncRemove）在执行前，先将输入的 key 按 shard 分组，然后逐个分片加锁处理：

```
1. 遍历所有 key，计算 shard_id = hash(key) % shard_count
2. 将 key 按 shard_id 分组
3. 对每个非空 shard：
   - 加对应类型的锁（读锁或写锁）
   - 处理该 shard 内的所有 key
   - 锁自动释放（RAII）
```

优势：
- 只锁定涉及的分片，不影响其他分片的并发访问
- 同一分片内的多个 key 在一次加锁中处理，减少锁开销
- 读写锁允许并发读，适合读多写少的 KV cache 场景

## 5. RPC 接口定义

### 5.1 Protobuf 定义

```protobuf
// falconkv_meta.proto

service FalconKVMetaService {
    rpc BatchExist(BatchExistRequest) returns (BatchExistResponse);
    rpc BatchLookup(BatchLookupRequest) returns (BatchLookupResponse);
    rpc SyncCommit(SyncCommitRequest) returns (SyncCommitResponse);
    rpc SyncRemove(SyncRemoveRequest) returns (SyncRemoveResponse);
    rpc StoreRegister(StoreRegisterRequest) returns (StoreRegisterResponse);
    rpc ClientHeartbeat(ClientHeartbeatRequest) returns (ClientHeartbeatResponse);
}

message KeyDesc {
    string key = 1;
    uint32 store_id = 2;
    uint64 offset = 3;
    uint32 size = 4;
    string data_file = 5;   // Store 数据文件路径（供同节点直读）
    string store_addr = 6;  // Store RPC 地址（供远程 RPC 用）
}
```

### 5.2 SyncCommit 处理流程

```
Meta 接收 SyncCommit:
1. 解析 request 中的 store_id 和 key_records
2. 对 key_records 按 shard 分组
3. 逐个分片加写锁，upsert 记录
4. 返回 SyncCommitResponse
```

### 5.3 SyncRemove 处理流程

```
Meta 接收 SyncRemove:
1. 解析 request 中的 store_id 和 keys
2. 对 keys 按 shard 分组
3. 逐个分片加写锁，删除记录
4. 返回 SyncRemoveResponse
```

## 6. falconkv_master 独立进程

Meta 模块以独立进程 `falconkv_master` 运行。该进程封装了 MetaServer 的创建与生命周期管理。

### 6.1 进程入口

`src/meta/falconkv_master_main.cpp` 负责：

1. 从命令行参数 / `FALCONKV_CONFIG_FILE` 环境变量 / 默认路径 `config/falconkv.json` 加载配置
2. 创建 `MetaServer(config.meta)` 并调用 `Start()`
3. 注册 SIGINT/SIGTERM 信号处理器，优雅退出
4. 主线程循环 `sleep(1)` 等待停止信号

```
falconkv_master 启动流程:
  加载配置 → 创建 MetaServer → Start() → 主循环 sleep(1) → 信号触发 → Stop() → 退出
```

### 6.2 CMake 构建

```cmake
# src/meta/CMakeLists.txt
add_executable(falconkv_master falconkv_master_main.cpp)
target_link_libraries(falconkv_master PRIVATE falconkv_meta Threads::Threads)
if(FALCONKV_HAS_BRPC)
    target_link_libraries(falconkv_master PRIVATE falconkv_brpc_deps)
endif()
install(TARGETS falconkv_master RUNTIME DESTINATION bin)
```

构建产出 `build/src/meta/falconkv_master`，安装到 `/usr/local/falconkv/bin/`。

## 7. Meta 断连容错

当 Meta 服务不可用时，Store 和 Client 仍可正常运行（降级为本地模式）。Meta 恢复后自动重连并完成全量重新同步。

### 7.1 MetaRpcClient（Client 侧）

Client 通过 `MetaRpcClient`（`src/meta/meta_rpc_client.h`）与 Meta 通信。断连容错行为：

| 行为 | 说明 |
|------|------|
| Connect 失败 | 不阻止 Client 启动，日志记录错误 |
| BatchExist / BatchLookup 断连 | 检测到 `!connected_` 立即返回空结果（不触发 RPC 超时） |
| RPC 失败 | `cntl.Failed()` 时设置 `connected_ = false`，触发后台重连 |
| 后台重连循环 | 每 5 秒尝试 `TryConnect()`，成功后标记 `connected_ = true` |

```
MetaRpcClient 断连时 BatchExist 流程:
  BatchExist(keys)
    │
    ├─ connected_ == true → 正常 RPC 查询
    │   └─ RPC 失败 → connected_ = false → 返回空结果
    │
    └─ connected_ == false → 直接返回空结果（跳过 RPC）
                                后台 ReconnectLoop 每 5s 尝试重连
```

```cpp
class MetaRpcClient {
public:
    Status Connect(const std::string& addr);
    std::vector<KeyRecord> BatchExist(const std::vector<std::string>& keys);
    std::vector<KeyRecord> BatchLookup(const std::vector<std::string>& keys);
    void StartReconnectLoop(int interval_sec);
    void StopReconnectLoop();
    bool connected() const;

private:
    std::unique_ptr<brpc::Channel> channel_;
    std::unique_ptr<FalconKVMetaService_Stub> stub_;
    std::atomic<bool> connected_{false};
    std::string meta_addr_;

    // Reconnect thread
    std::thread reconnect_thread_;
    std::atomic<bool> reconnect_running_{false};
    std::mutex reconnect_mutex_;
    std::condition_variable reconnect_cv_;
};
```

### 7.2 MetaSyncClient（Store 侧）

Store 通过 `MetaSyncClient`（`src/store/meta_sync_client.h`）向 Meta 推送元数据。断连容错行为：

| 行为 | 说明 |
|------|------|
| Connect 失败 | 不阻止 Store 启动，日志记录错误 |
| SyncCommit / SyncRemove / RegisterStore 断连 | 检测到 `!connected_` 跳过（返回 OK） |
| RPC 失败 | `cntl.Failed()` 时设置 `connected_ = false` |
| 后台重连循环 | 每 5 秒尝试 `TryConnect()`，成功后执行 `FullResync()` |
| FullResync | RegisterStore + 分批（256 条/批）SyncCommit 全量 committed key |

```
MetaSyncClient 重连 + 全量重同步流程:
  ReconnectLoop (每 5 秒)
    │
    ├─ connected_ == true → 跳过
    │
    └─ connected_ == false
        │
        ├─ TryConnect() → 成功
        │   │
        │   └─ FullResync()
        │       ├─ RegisterStore(store_id, data_file, capacity, chunk_size)
        │       └─ 分批 SyncCommit:
        │           entries = meta_index_->GetAllCommittedEntries()
        │           for batch(256) in entries:
        │             SyncCommit(store_id, batch)
        │           失败时中断，等待下次重连重试
        │
        └─ TryConnect() → 失败 → 等待下一个周期
```

```cpp
class MetaSyncClient {
public:
    Status Connect(const std::string& meta_addr);
    void SetStoreInfo(uint32_t store_id, const std::string& data_file,
                      uint64_t capacity_bytes, uint32_t chunk_size);
    void SetMetaIndex(StoreMetaIndex* meta_index);
    void StartReconnectLoop(int interval_sec);
    void StopReconnectLoop();

    // ... SyncCommit, SyncRemove, RegisterStore ...

private:
    Status TryConnect();
    void FullResync();
    void ReconnectLoop(int interval_sec);

    std::unique_ptr<brpc::Channel> channel_;
    std::unique_ptr<FalconKVMetaService_Stub> stub_;
    std::atomic<bool> connected_{false};
    std::string meta_addr_;

    // Store registration info
    uint32_t store_id_ = 0;
    std::string data_file_;
    uint64_t capacity_bytes_ = 0;
    uint32_t chunk_size_ = 0;
    StoreMetaIndex* meta_index_ = nullptr;  // not owned

    // Reconnect thread
    std::thread reconnect_thread_;
    std::atomic<bool> reconnect_running_{false};
    // ...
};
```

### 7.3 全量重新同步依赖：GetAllCommittedEntries()

`StoreMetaIndex` 提供新方法 `GetAllCommittedEntries()`，返回所有 `stat == 1` 的记录，用于 Meta 重连后的全量推送：

```cpp
// src/store/store_meta_index.h
class StoreMetaIndex {
public:
    // ... existing methods ...

    /// Return all committed entries (stat == 1), used for full resync.
    std::vector<StoreKeyRecord> GetAllCommittedEntries() const;
};
```

### 7.4 容错场景总览

```
场景 1: Meta 未启动，Store/Client 正常启动
  Store.Init():
    meta_sync_client_->Connect(addr)  → 失败，日志记录，不阻止启动
    meta_sync_client_->SetStoreInfo(...)
    meta_sync_client_->SetMetaIndex(...)
    meta_sync_client_->StartReconnectLoop(5)  → 后台尝试重连

  Client 构造:
    meta_client_.Connect(addr)  → 失败，日志记录
    meta_client_.StartReconnectLoop(5)  → 后台尝试重连

场景 2: Meta 启动后 Store 自动连接
  Store ReconnectLoop:
    TryConnect() → 成功
    FullResync() → RegisterStore + 分批 SyncCommit 全量 key

场景 3: Client 断连期间 BatchExist/BatchGet
  BatchExist:
    meta_client_.BatchExist(missing_keys)  → 返回空结果
    → Client 只看到本地 Store 的数据（降级为单机模式）

场景 4: Meta 运行中故障
  Client: RPC 失败 → connected_ = false → 后续请求早返空结果
  Store:  RPC 失败 → connected_ = false → SyncCommit 跳过
  双方:   ReconnectLoop 自动重连
```

## 8. 性能优化要点

| 优化手段 | 说明 |
|----------|------|
| 分片哈希表 | 64 个独立分片降低锁争用 |
| 分片读写锁 | 读操作可并发，适合读多写少场景 |
| 批量分片分组 | 同一分片的 key 一次加锁处理 |
| 纯内存存储 | 无磁盘 IO 和 PG 开销，纳秒级查找 |
| steady_clock | 时钟使用 steady_clock 避免系统时间跳变影响 |

## 9. 部署配置

> **注意**: Meta 监听地址由 `common.meta_addr`（默认 `0.0.0.0:18900`）统一配置，不属于 `meta.*` 专属字段。

| JSON 字段 (`meta.*`) | 默认值 | 说明 |
|------------------------|--------|------|
| `shard_count` | 64 | 元数据分片数 |
| `page_size` | 4096 | 页大小 |
| `heartbeat_timeout_sec` | 30 | Client 心跳超时（秒） |

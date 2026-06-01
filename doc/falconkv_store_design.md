# FalconKV Store 模块设计文档

## 1. 模块概述

Store 模块是 FalconKV 的数据持久化层，负责将 KV Cache 数据块高效地存储到 SSD 上，并提供低时延的读写能力。核心设计要点：

- **自管理空间**：Store 内部维护 BuddyAllocator 进行空间分配与回收，独立管理驱逐策略
- **Key-aware API**：提供基于 key 的 Put/Get/Contains/Remove 接口，调用方无需感知 offset
- **元数据同步**：通过 MetaSyncClient 将元数据变更异步同步到 Meta 服务器
- **大文件 + DirectIO**：预分配大文件，使用 DirectIO 直接操作 SSD，绕过 OS 页缓存
- **固定块管理**：KVCache 场景数据块大小固定，简化空间管理
- **同进程部署**：Store 与 Client 在同一进程空间，支持本地高速读写
- **零拷贝传输**：远程数据通过 brpc attachment 零拷贝传输

### 1.1 设计参考

- **FalconFS FalconStore**：基于 SSD 的存储引擎，使用 DirectIO 读写，文件按 `{inodeId}-large` 格式命名
- **Linux DirectIO**：绕过页缓存，直接操作块设备，需要内存对齐
- **Linux Buddy Allocator**：固定大小块的空间管理算法，适配 KVCache 固定块大小场景

### 1.2 核心约束

| 约束项 | 说明 |
|--------|------|
| 存储介质 | NVMe SSD |
| IO 模式 | DirectIO (O_DIRECT) |
| 文件系统 | XFS / ext4 (支持大文件) |
| 对齐要求 | 512 字节对齐（SSD 扇区大小） |
| 数据块大小 | 固定（由 LMCache chunk_size 决定） |
| 部署模式 | 与 Client 同进程或独立进程 |

## 2. 架构设计

### 2.1 整体架构

```
┌──────────────────────────────────────────────────────────────────────┐
│                    FalconKV Store Process                             │
│                                                                       │
│  ┌───────────────────────────────────────────────────────────────┐   │
│  │ StoreServer (封装启动/停止)                                    │   │
│  │  ┌─────────────────────────────────────────────────────────┐  │   │
│  │  │ StoreServiceImpl (FalconKVStoreService RPC 处理)         │  │   │
│  │  │  - Write / Read / BatchWrite / BatchRead / Ping          │  │   │
│  │  │  - 委托给 FalconKVStore 执行实际 IO                      │  │   │
│  │  └───────────────────────────┬─────────────────────────────┘  │   │
│  │                              │ brpc::Server                    │   │
│  └──────────────────────────────┼────────────────────────────────┘   │
│                              │                                       │
│  ┌───────────────────────────┼───────────────────────────────────┐   │
│  │ Store Engine              │                                    │   │
│  │                           ▼                                    │   │
│  │  ┌─────────────────────────────────────────────────────────┐  │   │
│  │  │ FalconKVStore                                            │  │   │
│  │  │  - Key-aware API: Put/Get/Contains/Remove/BatchPut/...   │  │   │
│  │  │  - DirectIO: Write/Read (含对齐处理)                     │  │   │
│  │  │  - BatchWrite/BatchRead (排序 + 并行)                    │  │   │
│  │  │                                                          │  │   │
│  │  │  内部组件:                                                │  │   │
│  │  │  ┌──────────────────┐  ┌──────────────────────────────┐ │  │   │
│  │  │  │ BuddyAllocator   │  │ StoreMetaIndex               │ │  │   │
│  │  │  │ (空间分配/回收)  │  │ (本地 key→offset 索引)       │ │  │   │
│  │  │  └──────────────────┘  └──────────────────────────────┘ │  │   │
│  │  │  ┌────────────────────────────────────────────────────┐ │  │   │
│  │  │  │ MetaSyncClient (异步同步元数据到 Meta 服务器)       │ │  │   │
│  │  │  │  - SyncCommit() / SyncRemove()                     │ │  │   │
│  │  │  └────────────────────────────────────────────────────┘ │  │   │
│  │  │  ┌────────────────────────────────────────────────────┐ │  │   │
│  │  │  │ PendingEvictQueue (驱逐宽限期队列, 5s)             │ │  │   │
│  │  │  │  - 先删 Meta 再延迟回收本地空间                     │ │  │   │
│  │  │  └────────────────────────────────────────────────────┘ │  │   │
│  │  └─────────────────────────────────────────────────────────┘  │   │
│  └───────────────────────────────────────────────────────────────┘   │
│                              │                                       │
│  ┌───────────────────────────┼───────────────────────────────────┐   │
│  │ Storage Layer             │                                    │   │
│  │                           ▼                                    │   │
│  │  ┌─────────────────────────────────────────────────────────┐  │   │
│  │  │ SSD: /data/falconkv/kv_data_{store_id}                   │  │   │
│  │  └─────────────────────────────────────────────────────────┘  │   │
│  └───────────────────────────────────────────────────────────────┘   │
└──────────────────────────────────────────────────────────────────────┘
```

### 2.2 核心类设计

```cpp
class FalconKVStore {
public:
    struct Config {
        std::string ssd_path;          // SSD 数据根目录
        uint32_t store_id;             // Store 唯一标识（从配置文件读取）
        uint64_t capacity_bytes;        // 总容量
        uint32_t chunk_size;            // 单个 KV 块大小
        uint32_t page_size = 4096;      // 页大小
        uint32_t io_threads = 4;        // IO 工作线程数
        uint32_t write_queue_size = 1024; // 写队列大小
        bool disable_mtime = true;      // 禁用文件修改时间更新
        std::string scheduler_uds_path; // IO Scheduler UDS 路径（可选）
        bool scheduler_enabled = true;  // 是否启用 Scheduler 上报
        std::string meta_addr;          // Meta 服务器地址
    };

    FalconKVStore(const Config& config);
    ~FalconKVStore();

    // 初始化：创建数据文件、初始化 BuddyAllocator、向 Meta 注册
    Status Init();

    // ===== Key-aware API (供 Client 调用) =====

    // 写入单个 key
    Status Put(const std::string& key, const void* data, uint32_t size);

    // 读取单个 key
    Status Get(const std::string& key, void* buffer, uint32_t size);

    // 查询 key 是否存在
    bool Contains(const std::string& key);

    // 删除 key
    Status Remove(const std::string& key);

    // 批量写入
    std::vector<Status> BatchPut(const std::vector<std::string>& keys,
                                  const std::vector<const void*>& data_ptrs,
                                  const std::vector<uint32_t>& sizes);

    // 批量读取
    std::vector<Status> BatchGet(const std::vector<std::string>& keys,
                                  const std::vector<void*>& buffers,
                                  const std::vector<uint32_t>& sizes);

    // 批量查询存在性
    // 返回命中数量，命中/未命中的 key 列表
    int BatchContains(const std::vector<std::string>& keys,
                       std::vector<std::string>& hit_keys,
                       std::vector<std::string>& missing_keys);

    // ===== Offset-based API (供 RPC 服务使用) =====

    // 按偏移写入数据
    Status Write(uint64_t offset, const void* data, uint32_t size);

    // 按偏移读取数据
    Status Read(uint64_t offset, void* buffer, uint32_t size);

    // 批量写入
    Status BatchWrite(const std::vector<WriteItem>& items);

    // 批量读取
    Status BatchRead(const std::vector<ReadItem>& items);

    // 关闭
    void Close();

private:
    Config config_;
    int data_fd_;                       // 数据文件 fd (O_DIRECT)
    uint32_t store_id_;                 // 从 config_.store_id 初始化
    std::string data_file_;             // 完整数据文件路径（含 store_id）

    // 空间管理
    std::unique_ptr<BuddyAllocator> allocator_;

    // 本地元数据索引
    std::unique_ptr<StoreMetaIndex> meta_index_;

    // Meta 同步客户端
    std::unique_ptr<MetaSyncClient> meta_sync_client_;

    // 驱逐宽限期队列（先删 Meta 元数据，延迟 5s 后回收本地空间）
    std::unique_ptr<PendingEvictQueue> pending_evict_queue_;

    // IO 工作线程池
    std::vector<std::thread> io_workers_;

    // 写入队列
    moodycamel::BlockingConcurrentQueue<WriteTask> write_queue_;

    // Meta 通道（用于注册和心跳）
    std::unique_ptr<TransferChannel> meta_channel_;

    // IO Scheduler 代理（可选）
    std::unique_ptr<SchedulerProxy> scheduler_proxy_;
};
```

### 2.3 StoreMetaIndex（本地元数据索引）

Store 维护本地 key→offset 的 hash 索引，支持快速本地查找，避免每次操作都查询 Meta。

```cpp
class StoreMetaIndex {
public:
    // 插入/更新 key 记录
    void Put(const std::string& key, const StoreKeyRecord& record);

    // 查找已提交的 key
    std::optional<StoreKeyRecord> Get(const std::string& key);

    // 批量查询存在性
    // hits: 存在的 key 列表，misses: 不存在的 key 列表
    void BatchContains(const std::vector<std::string>& keys,
                        std::vector<std::string>& hits,
                        std::vector<std::string>& misses);

    // 标记 key 为已提交（stat 0→1）
    void Commit(const std::string& key);

    // 删除 key 记录
    void Remove(const std::string& key);

    // 更新访问时间
    void Touch(const std::string& key);

    // 获取所有已提交记录（用于 Meta 重连后全量重新同步）
    std::vector<StoreKeyRecord> GetAllCommittedEntries() const;

private:
    std::mutex mutex_;
    std::unordered_map<std::string, StoreKeyRecord> index_;
};

struct StoreKeyRecord {
    uint64_t offset;             // 数据偏移
    uint32_t size;               // 数据大小
    uint8_t stat;                // 状态：0=writing, 1=committed
    uint64_t access_time_ms;     // 最后访问时间
};
```

### 2.4 MetaSyncClient（Meta 同步客户端 + 断连容错）

Store 通过 MetaSyncClient 将元数据变更异步同步到 Meta 服务器。支持 Meta 不可用时的断连容错和自动重连。

```cpp
class MetaSyncClient {
public:
    // 连接 Meta 服务器（失败不阻止 Store 启动）
    Status Connect(const std::string& meta_addr);

    // 设置注册信息（用于重连后重新注册）
    void SetStoreInfo(uint32_t store_id, const std::string& data_file,
                      uint64_t capacity_bytes, uint32_t chunk_size);

    // 设置本地索引指针（用于全量重同步，不拥有所有权）
    void SetMetaIndex(StoreMetaIndex* meta_index);

    // 启动后台重连循环（每 interval_sec 秒检查）
    void StartReconnectLoop(int interval_sec);

    // 停止后台重连循环
    void StopReconnectLoop();

    // 同步已提交的 key 到 Meta（断连时跳过）
    Status SyncCommit(uint32_t store_id,
                       const std::vector<StoreKeyRecord>& records);

    // 同步已删除/驱逐的 key 到 Meta（断连时跳过）
    Status SyncRemove(uint32_t store_id,
                       const std::vector<std::string>& keys);

    // 向 Meta 注册 Store（断连时跳过）
    Status RegisterStore(uint32_t store_id, const std::string& data_file,
                         uint64_t capacity_bytes, uint32_t chunk_size);

    bool connected() const;

private:
    Status TryConnect();       // 内部重连
    void FullResync();         // 重连后全量重新同步
    void ReconnectLoop(int interval_sec);  // 后台重连线程

    std::unique_ptr<brpc::Channel> channel_;
    std::unique_ptr<FalconKVMetaService_Stub> stub_;
    std::atomic<bool> connected_{false};
    std::string meta_addr_;

    // Store 注册信息
    uint32_t store_id_ = 0;
    std::string data_file_;
    uint64_t capacity_bytes_ = 0;
    uint32_t chunk_size_ = 0;
    StoreMetaIndex* meta_index_ = nullptr;  // not owned

    // 重连线程
    std::thread reconnect_thread_;
    std::atomic<bool> reconnect_running_{false};
    std::mutex reconnect_mutex_;
    std::condition_variable reconnect_cv_;
};
```

**断连容错行为**：

| 行为 | 说明 |
|------|------|
| Connect 失败 | 不阻止 Store 启动，日志记录错误 |
| SyncCommit/SyncRemove/RegisterStore 断连 | 检测到 `!connected_` 跳过（返回 OK） |
| RPC 失败 | `cntl.Failed()` 时设置 `connected_ = false`，触发后台重连 |
| 后台重连循环 | 每 5 秒尝试 TryConnect()，成功后执行 FullResync() |
| FullResync | RegisterStore + 分批（256 条/批）SyncCommit 全量 committed key |

**Store.Init() 初始化顺序**：

```cpp
FalconKVStore::Init():
    // ...
    meta_sync_client_ = std::make_unique<MetaSyncClient>();
    meta_sync_client_->Connect(addr);            // 失败也 OK
    meta_sync_client_->SetStoreInfo(store_id_, data_file_, capacity, chunk_size);
    meta_sync_client_->SetMetaIndex(meta_index_.get());
    meta_sync_client_->StartReconnectLoop(5);    // 后台重连
    // ...

FalconKVStore::Close():
    // ...
    if (meta_sync_client_) {
        meta_sync_client_->StopReconnectLoop();  // 先停止重连线程
    }
    meta_sync_client_.reset();
    // ...
```

### 2.5 PendingEvictQueue（驱逐宽限期队列）

驱逐数据时，Store 必须先通知 Meta 删除元数据，然后将待驱逐记录移入 `PendingEvictQueue`，等待宽限期（5 秒）后再回收本地空间。这确保了：

1. **Meta 元数据先行删除**：其他 Client 的 BatchExist 查询不会再命中这些 key
2. **宽限期兜底**：在 Meta 删除到本地回收之间的窗口期，已经获取到旧 key 描述的 Client 仍可成功读取
3. **避免读取不可预知数据**：宽限期过后空间才真正回收，杜绝读到被覆盖的部分数据

```cpp
class PendingEvictQueue {
public:
    PendingEvictQueue(BuddyAllocator* allocator, StoreMetaIndex* meta_index);

    // 将待驱逐记录加入队列（元数据已通过 SyncRemove 在 Meta 中删除）
    void Enqueue(const std::vector<PendingEvictItem>& items);

    // 后台线程：检查超过宽限期的记录，回收空间
    void Start();
    void Stop();

private:
    void EvictLoop();

    struct PendingEvictItem {
        std::string key;
        uint64_t offset;           // BuddyAllocator 中的偏移
        uint32_t size;             // 数据大小
        uint64_t enqueue_time_ms;  // 入队时间（毫秒时间戳）
    };

    std::mutex mutex_;
    std::deque<PendingEvictItem> queue_;  // 按入队时间有序
    std::condition_variable cv_;
    std::thread evict_thread_;
    std::atomic<bool> running_{false};

    BuddyAllocator* allocator_;     // not owned
    StoreMetaIndex* meta_index_;    // not owned

    static constexpr uint64_t GRACE_PERIOD_MS = 5000;  // 5 秒宽限期
};
```

### 2.6 BuddyAllocator（空间管理）

BuddyAllocator 位于 `src/common/`，被 Store 和 Meta（测试用）共同使用。每个 Store 实例维护一个独立的 BuddyAllocator。

```cpp
class BuddyAllocator {
public:
    BuddyAllocator(uint64_t total_bytes, uint32_t page_size, uint32_t chunk_pages);

    // 分配一个 chunk（chunk_pages 个连续页）
    // 返回起始偏移（字节），失败返回 -1
    int64_t AllocChunk();

    // 释放一个 chunk
    void FreeChunk(int64_t offset);

    // 获取使用率（0.0 ~ 1.0）
    double GetUsageRatio() const;

    // 获取总容量和已使用量
    uint64_t GetTotalBytes() const;
    uint64_t GetUsedBytes() const;

private:
    uint32_t page_size_;        // 页大小（4096）
    uint32_t chunk_pages_;      // 每个 chunk 占用页数
    uint32_t total_pages_;      // 总页数
    uint32_t used_pages_;       // 已使用页数

    // 位图：每个 bit 代表一个页是否已分配
    std::vector<uint64_t> page_bitmap_;

    // Buddy 空闲链表：free_list_[order] 存储该阶的空闲块列表
    // order=0: 1 page, order=1: 2 pages, ..., order=n: 2^n pages
    std::vector<std::list<uint32_t>> free_list_;

    // 分配锁
    std::mutex mutex_;
};
```

#### 分配流程

```
AllocChunk():
  1. lock_guard(mutex_)
  2. 计算 target_order = ceil(log2(chunk_pages_))
  3. 从 free_list_[target_order] 取出空闲块
  4. 若无空闲块:
     a. 从更高阶分裂（如 order+1 分裂为两个 order）
     b. 递归直到找到可用的阶
  5. 标记 page_bitmap_ 对应位为已使用
  6. 更新 used_pages_
  7. 返回 offset = page_start * page_size_
```

#### 回收流程

```
FreeChunk(offset):
  1. lock_guard(mutex_)
  2. 计算 page_start = offset / page_size_
  3. 标记 page_bitmap_ 对应位为空闲
  4. 更新 used_pages_
  5. 尝试 Buddy 合并:
     a. 计算 buddy 页号 = page_start ^ chunk_pages_
     b. 若 buddy 也空闲: 合并为更大块
     c. 递归合并直到无法合并
  6. 将合并后的空闲块加入对应 free_list_
```

### 2.6 Store RPC Service 层

Client-Store 分离后，Store 模块通过 RPC Service 层提供远程访问能力。

**StoreServiceImpl** — 服务端 RPC 处理器：

```cpp
class StoreServiceImpl : public FalconKVStoreService {
public:
    explicit StoreServiceImpl(FalconKVStore* store);

    void Write(RpcController*, const WriteRequest*, WriteResponse*, Closure*) override;
    void Read(RpcController*, const ReadRequest*, ReadResponse*, Closure*) override;
    void BatchWrite(RpcController*, const BatchWriteRequest*, BatchWriteResponse*, Closure*) override;
    void BatchRead(RpcController*, const BatchReadRequest*, BatchReadResponse*, Closure*) override;
    void Ping(RpcController*, const PingRequest*, PongResponse*, Closure*) override;

private:
    FalconKVStore* store_;  // not owned
};
```

**StoreServer** — 独立 Store 进程封装：

```cpp
class StoreServer {
public:
    StoreServer(const FalconKVStore::Config& store_config, const std::string& listen_addr);
    Status Start();   // Init Store + AddService + brpc::Server::Start
    void Stop();      // brpc::Server::Stop + Store::Close

    FalconKVStore* GetStore();
    const std::string& ListenAddr() const;

private:
    FalconKVStore store_;
    StoreServiceImpl service_impl_;
    brpc::Server server_;
};
```

**StoreRpcClient** — 客户端 RPC 封装：

```cpp
class StoreRpcClient {
public:
    Status Connect(const std::string& addr);
    Status Write(uint64_t offset, const void* data, uint32_t size);
    Status Read(uint64_t offset, void* buffer, uint32_t size);
    Status Ping();

private:
    brpc::Channel channel_;
    std::unique_ptr<FalconKVStoreService_Stub> stub_;
};
```

**StoreRpcClientManager** — 按地址缓存的连接池：

```cpp
class StoreRpcClientManager {
public:
    StoreRpcClient* GetOrCreate(const std::string& addr);
    void CloseAll();

private:
    std::unordered_map<std::string, std::unique_ptr<StoreRpcClient>> clients_;
};
```

**NodeLocalAccessor** — 同节点文件直通读写器：

```cpp
class NodeLocalAccessor {
public:
    void RegisterStoreFile(uint32_t store_id, const std::string& data_file);
    Status Write(uint32_t store_id, uint64_t offset, const void* data, uint32_t size);
    Status Read(uint32_t store_id, uint64_t offset, void* buffer, uint32_t size);
    void Close();

private:
    int GetFd(uint32_t store_id);
    std::unordered_map<uint32_t, std::string> store_files_;  // store_id → data_file
    FdCache fd_cache_;  // 内部使用
};
```

## 3. Key-aware API 内部流程

### 3.1 Put 内部流程

```
Store.Put(key, data, size):
1. allocator_->AllocChunk() → offset        // BuddyAllocator 分配空间
2. Write(offset, data, size)                 // DirectIO 写入 SSD
3. meta_index_->Put(key, {offset, size, stat=1})  // 更新本地索引
4. meta_sync_client_->SyncCommit(store_id, {record}) // 异步同步到 Meta
```

### 3.2 Get 内部流程

```
Store.Get(key, buffer, buffer_size):
1. meta_index_->Get(key) → StoreKeyRecord    // 查询本地索引
2. Read(record.offset, buffer, record.size)  // DirectIO 从 SSD 读取
3. meta_index_->Touch(key)                   // 更新访问时间
```

### 3.3 BatchPut 内部流程

```
Store.BatchPut(keys, data_ptrs, sizes):
1. 批量从 allocator_ 分配空间 → offsets[]
2. 排序写入请求（按 offset），优化顺序 IO
3. 批量 Write 到 SSD
4. 批量更新 meta_index_
5. meta_sync_client_->SyncCommit(store_id, records[]) // 批量异步同步
```

### 3.4 BatchContains 内部流程

```
Store.BatchContains(keys, hits, misses):
1. 遍历 keys，查询 meta_index_
2. 存在且 stat=1 的 key → hits
3. 不存在或 stat≠1 的 key → misses
4. 返回 hits.size()
```

### 3.5 驱逐流程（先删 Meta + 宽限期回收）

Store 内部管理驱逐策略，当使用率超过高水位线时触发。驱逐流程遵循**先删元数据、延迟回收空间**的原则：

```
EvictLoop (后台线程):
1. 检查 allocator_->GetUsageRatio()
2. 若超过 high_watermark (0.85):
   a. 从 meta_index_ 中按 access_time_ms 排序找冷数据候选
   b. ★ 先通知 Meta 删除元数据:
      meta_sync_client_->SyncRemove(store_id, evict_keys[])
      → 等待 Meta 确认删除成功
      → 此时其他 Client 的 BatchExist 不再命中这些 key
   c. 将候选记录移入 pending_evict_queue_（记录入队时间）
      → 数据仍在 SSD 上，已拿到旧 key 描述的 Client 仍可读取
   d. pending_evict_queue_ 后台线程检查:
      - 超过 5 秒宽限期的记录 → allocator_->FreeChunk(offset)
      - 从 meta_index_ 中移除对应 key
3. sleep(evict_interval_)
```

**驱逐时序保证**：

```
时间线:
──────────────────────────────────────────────────────────────────▶

T0: Store 选取冷数据候选
T1: Store → Meta SyncRemove(keys)          ← Meta 删除元数据
    │                                         BatchExist 不再返回这些 key
T2: Store 将候选移入 PendingEvictQueue      ← 数据仍在 SSD，可读
    │
    │   ← 宽限期 (5s) →                      ← 已获取旧描述的 Client 仍可成功读取
    │
T3: Store 回收空间                           ← BuddyAllocator.FreeChunk
    StoreMetaIndex.Remove(key)              ← 空间可被新数据覆盖
```

**设计要点**：
- **Meta 先行删除**：防止新的 Client 获取已驱逐数据的 key 描述
- **5 秒宽限期**：为已经获取到 key 描述的 Client 提供读取窗口
- **空间延迟回收**：宽限期后才释放 BuddyAllocator 空间，确保读取不会遇到不可预知的数据

## 4. 数据文件管理

### 4.1 文件创建与初始化

每个 Store 实例使用独立的数据文件，文件名包含 `store_id` 以避免多 Store 部署在同一 SSD 目录时产生冲突。

```
文件命名规则:
  {ssd_path}/kv_data_{store_id}

示例:
  store_id=0 → /data/falconkv/kv_data_0
  store_id=1 → /data/falconkv/kv_data_1
  store_id=7 → /data/falconkv/kv_data_7

同节点多 Store 共存:
  /data/falconkv/
  ├── kv_data_0    (GPU 0 Store, 500GB)
  ├── kv_data_1    (GPU 1 Store, 500GB)
  ├── ...
  └── kv_data_7    (GPU 7 Store, 500GB)

生产环境 store_id 生成规则:
  store_id = hash(node_id) * max_gpu_per_node + gpu_id
  例: node_id=2, gpu_id=3, max_gpu=8 → store_id = 19
```

```cpp
Status FalconKVStore::InitDataFile() {
    // 文件名拼接 store_id，每个 Store 使用独立数据文件
    data_file_ = fmt::format("{}/kv_data_{}", config_.ssd_path, store_id_);

    // 创建文件
    data_fd_ = open(data_file_.c_str(),
                     O_CREAT | O_RDWR | O_DIRECT,  // DirectIO
                     0644);

    if (data_fd_ < 0) {
        return Status::IOError(
            fmt::format("Failed to create data file: {}", data_file_));
    }

    // 预分配空间（fallocate）
    int ret = fallocate(data_fd_,
                        FALLOC_FL_KEEP_SIZE | FALLOC_FL_PUNCH_HOLE,
                        0,
                        config_.capacity_bytes);

    if (ret != 0) {
        return Status::IOError("Failed to fallocate");
    }

    // 禁用文件访问时间更新
    int flags = fcntl(data_fd_, F_GETFL);
    fcntl(data_fd_, F_SETFL, flags | O_NOATIME);

    // 初始化 BuddyAllocator
    allocator_ = std::make_unique<BuddyAllocator>(
        config_.capacity_bytes, config_.page_size,
        config_.chunk_size / config_.page_size);

    return Status::OK();
}
```

### 4.2 DirectIO 读写

```cpp
// DirectIO 写入
Status FalconKVStore::Write(uint64_t offset, const void* data, uint32_t size) {
    // DirectIO 要求：
    // 1. buffer 地址按 512 字节对齐
    // 2. offset 按 512 字节对齐
    // 3. size 是 512 字节的整数倍

    // 1. 准备对齐 buffer
    uint32_t aligned_size = AlignUp(size, 512);
    void* aligned_buf = nullptr;

    if (IsAligned(data, 512) && aligned_size == size) {
        // 数据已对齐，直接使用
        aligned_buf = const_cast<void*>(data);
    } else {
        // 需要对齐拷贝
        aligned_buf = AlignedAlloc(512, aligned_size);
        memset(aligned_buf, 0, aligned_size);
        memcpy(aligned_buf, data, size);
    }

    // 2. 执行 pwrite (DirectIO)
    ssize_t bytes_written = pwrite(data_fd_, aligned_buf, aligned_size, offset);

    if (aligned_buf != data) {
        AlignedFree(aligned_buf);
    }

    if (bytes_written != static_cast<ssize_t>(aligned_size)) {
        return Status::IOError("pwrite failed");
    }

    return Status::OK();
}

// DirectIO 读取
Status FalconKVStore::Read(uint64_t offset, void* buffer, uint32_t size) {
    uint32_t aligned_size = AlignUp(size, 512);
    void* read_buf = nullptr;

    // 如果用户 buffer 不对齐，使用对齐的中间 buffer
    if (IsAligned(buffer, 512)) {
        read_buf = buffer;
    } else {
        read_buf = AlignedAlloc(512, aligned_size);
    }

    // 执行 pread (DirectIO)
    ssize_t bytes_read = pread(data_fd_, read_buf, aligned_size, offset);

    if (read_buf != buffer) {
        memcpy(buffer, read_buf, size);
        AlignedFree(read_buf);
    }

    if (bytes_read != static_cast<ssize_t>(aligned_size)) {
        return Status::IOError("pread failed");
    }

    return Status::OK();
}
```

### 4.3 批量读写

```cpp
Status FalconKVStore::BatchWrite(const std::vector<WriteItem>& items) {
    // 对多个写入请求进行排序（按 offset），优化顺序 IO
    std::vector<size_t> sorted_indices(items.size());
    std::iota(sorted_indices.begin(), sorted_indices.end(), 0);
    std::sort(sorted_indices.begin(), sorted_indices.end(),
              [&items](size_t a, size_t b) {
                  return items[a].offset < items[b].offset;
              });

    // 顺序写入
    for (size_t idx : sorted_indices) {
        const auto& item = items[idx];
        auto s = Write(item.offset, item.data, item.size);
        if (!s.OK()) {
            return s;
        }
    }
    return Status::OK();
}

Status FalconKVStore::BatchRead(const std::vector<ReadItem>& items) {
    // 并行 pread
    std::vector<std::future<Status>> futures;
    for (const auto& item : items) {
        futures.push_back(std::async(std::launch::async, [this, &item]() {
            return Read(item.offset, item.buffer, item.size);
        }));
    }

    for (auto& f : futures) {
        auto s = f.get();
        if (!s.OK()) return s;
    }
    return Status::OK();
}
```

## 5. 内存对齐管理

### 5.1 对齐分配器

```cpp
class AlignedAllocator {
public:
    // 分配按 alignment 对齐的内存
    static void* Allocate(size_t alignment, size_t size) {
        void* ptr = nullptr;
        int ret = posix_memalign(&ptr, alignment, size);
        if (ret != 0) return nullptr;
        return ptr;
    }

    static void Free(void* ptr) {
        free(ptr);
    }

    // 检查地址是否对齐
    static bool IsAligned(const void* ptr, size_t alignment) {
        return (reinterpret_cast<uintptr_t>(ptr) % alignment) == 0;
    }

    // 向上对齐
    static size_t AlignUp(size_t value, size_t alignment) {
        return (value + alignment - 1) & ~(alignment - 1);
    }
};

// 对齐 buffer 池（避免频繁分配）
class AlignedBufferPool {
public:
    AlignedBufferPool(size_t buffer_size, size_t alignment, size_t pool_size)
        : buffer_size_(buffer_size), alignment_(alignment) {
        for (size_t i = 0; i < pool_size; i++) {
            auto* buf = AlignedAllocator::Allocate(alignment, buffer_size);
            if (buf) free_buffers_.push(buf);
        }
    }

    void* Get() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (free_buffers_.empty()) {
            return AlignedAllocator::Allocate(alignment_, buffer_size_);
        }
        auto* buf = free_buffers_.top();
        free_buffers_.pop();
        return buf;
    }

    void Put(void* buf) {
        std::lock_guard<std::mutex> lock(mutex_);
        free_buffers_.push(buf);
    }

    ~AlignedBufferPool() {
        while (!free_buffers_.empty()) {
            AlignedAllocator::Free(free_buffers_.top());
            free_buffers_.pop();
        }
    }

private:
    size_t buffer_size_;
    size_t alignment_;
    std::stack<void*> free_buffers_;
    std::mutex mutex_;
};
```

## 6. Store 注册与心跳

### 6.1 启动流程

```
Store 启动:
1. 读取配置文件（ssd_path, store_id, capacity, chunk_size, meta_addr）
2. 初始化 store_id_ = config_.store_id
3. 创建/打开数据文件（O_DIRECT）
   - 文件名: {ssd_path}/kv_data_{store_id}
4. fallocate 预分配空间
5. 初始化 BuddyAllocator
6. 初始化 StoreMetaIndex
7. 初始化 PendingEvictQueue（宽限期驱逐队列）
8. 初始化 MetaSyncClient:
   a. Connect(meta_addr) — 失败不阻止启动，日志记录错误
   b. SetStoreInfo(store_id, data_file, capacity, chunk_size) — 保存注册信息
   c. SetMetaIndex(meta_index_) — 设置本地索引指针
   d. StartReconnectLoop(5) — 启动后台重连线程
   e. 若初始连接成功，则立即 RegisterStore
9. 通过 MetaSyncClient 向 Meta 发送 StoreRegister RPC:
   - node_host, node_port
   - ssd_path
   - data_file（完整路径，供同节点 Client 直读）
   - capacity_bytes
   - chunk_size
   - store_id
10. 启动 RPC Server（监听 Store 服务端口）
11. 启动心跳线程（定期向 Meta 发送心跳）
12. 启动驱逐线程（定期检查使用率，触发驱逐）
13. 启动 IO Worker 线程池
14. 进入服务状态
```

### 6.2 心跳机制

```cpp
class StoreHeartbeat {
public:
    void Start(uint32_t store_id, const std::string& meta_addr) {
        thread_ = std::thread(&StoreHeartbeat::Loop, this,
                              store_id, meta_addr);
    }

    void Stop() {
        running_ = false;
        if (thread_.joinable()) thread_.join();
    }

private:
    void Loop(uint32_t store_id, const std::string& meta_addr) {
        auto channel = TransferChannelFactory::Create("brpc", config_);

        while (running_) {
            std::this_thread::sleep_for(heartbeat_interval_);

            StoreHeartbeatRequest request;
            request.set_store_id(store_id);
            request.set_used_bytes(allocator_->GetUsedBytes());
            request.set_capacity_bytes(allocator_->GetTotalBytes());

            StoreHeartbeatResponse response;
            auto status = channel->SyncCall(request, &response);

            if (!status.OK()) {
                LOG(WARNING) << "Heartbeat to Meta failed: " << status.ToString();
            }
        }
    }

    std::chrono::seconds heartbeat_interval_{5};
    std::thread thread_;
    std::atomic<bool> running_{true};
};
```

## 7. IO 路径优化

### 7.1 写入路径优化

```
标准写入路径:
  RPC 接收 → 反序列化 → WriteQueue → IO Worker → pwrite → 回复

优化点:
1. 写入合并: 相邻 offset 的写入合并为一次大 IO
2. 写入批量化: 多个小写入合并为一次 pwritev
3. 异步写入: 写入提交到队列后立即回复
4. IO 优先级: 读取优先于写入（避免读取被大量写入阻塞）
```

### 7.2 读取路径优化

```
标准读取路径:
  RPC 接收 → 反序列化 → pread → 序列化 → RPC 回复

优化点:
1. 同步读取: 直接在 RPC handler 中执行 pread（低延迟）
2. 零拷贝: 使用 brpc attachment 传输大数据，避免 protobuf 序列化
3. IO_URING: 使用 io_uring 批量提交读取请求（未来优化）
4. 读缓存: 热点数据可选的 DRAM 缓存层（未来优化）
```

### 7.3 IO_URING 预留

```cpp
// 未来可使用 io_uring 替代 pread/pwrite
class IoUringEngine {
public:
    Status Init(unsigned int entries);
    Status SubmitRead(int fd, void* buf, size_t size, off_t offset);
    Status SubmitWrite(int fd, const void* data, size_t size, off_t offset);
    Status WaitCompletion(unsigned int nr);
};
```

## 8. IO Scheduler 上报

### 8.1 Store 上报职责

Store 节点在接收到**远程 Client** 的 RPC 读写请求时，需要向 IO Scheduler 上报 IO 信息。本地 Client 的 IO 由 Client 自行上报，避免重复统计。

```
上报范围:
┌──────────────────────────────────────────────────────────┐
│ 本地 Client IO                                            │
│  Client 0 → Store 0 (同进程)                              │
│  → Client 0 自己向 Scheduler 上报，Store 不重复上报        │
├──────────────────────────────────────────────────────────┤
│ 远程 Client IO                                            │
│  Client X (远程) → RPC → Store 0                          │
│  → Store 0 向 Scheduler 上报（Store 侧视角的 IO 统计）     │
└──────────────────────────────────────────────────────────┘
```

### 8.2 上报实现

Store 在接收到远程 Client 的 RPC 请求时，使用 NET_RX 通道向 Scheduler 上报 IO 信息。

```
Store 上报 IOChannel 映射:
┌───────────────┬──────────────────────────────────────────────────────┐
│ RPC 请求类型  │ IOChannel 映射                                       │
├───────────────┼──────────────────────────────────────────────────────┤
│ 远程 Read RPC │ NET_RX_READ（网络接收请求 + SSD 读取 + 网络回传）     │
└───────────────┴──────────────────────────────────────────────────────┘
```

```cpp
// Store RPC Handler 中嵌入 Scheduler 上报
void FalconKVStore::HandleRPCRead(
    const ReadRequest& request,
    ReadResponse* response) {

    uint64_t start_ts = GetCurrentTimeNs();

    // 1. 执行实际 DirectIO 读取
    auto status = Read(request.offset(), buffer, request.size());

    uint64_t done_ts = GetCurrentTimeNs();

    // 2. 向 Scheduler 上报（仅远程请求）
    if (scheduler_proxy_ && !scheduler_proxy_->IsBypassMode()
        && IsRemoteRequest(request)) {
        StoreIOReport report;
        report.set_store_id(store_id_);
        report.set_io_channel(NET_RX_READ);
        report.set_source_client_id(request.client_id());
        report.set_io_size(request.size());
        report.set_request_ts_ns(start_ts);
        report.set_done_ts_ns(done_ts);
        report.set_source_node_addr(GetRemoteNodeAddr(request));

        // 异步上报，fire-and-forget，不阻塞 RPC 响应
        scheduler_proxy_->StoreReportIOAsync(report);
    }

    // 3. 填充并返回 RPC 响应
    // ...
}
```

### 8.3 Bypass 行为

Store 端的 Scheduler 上报使用与 Client 端相同的 `SchedulerProxy`，具备相同的 bypass 能力：

| 状态 | Store 行为 |
|------|-----------|
| Scheduler 正常 | IO 完成后异步上报，不阻塞响应 |
| Scheduler 不可达 | 连续 3 次上报失败后标记 bypass，不再尝试 |
| bypass 模式中 | 后台线程定期探测 Scheduler，自动恢复 |

### 8.4 上报对 Store 性能的影响

| 操作 | 额外开销 | 说明 |
|------|----------|------|
| Scheduler 上报 RPC | < 50us | UDS 异步 fire-and-forget |
| bypass 检测 | < 1us | 原子变量读取 |
| bypass 恢复探测 | 无 | 后台独立线程 |

## 9. 文件元数据优化

### 9.1 禁用文件时间更新

```cpp
// 方法 1: 挂载文件系统时使用 noatime 选项
// mount -o noatime /dev/nvme0n1 /data

// 方法 2: 使用 fcntl 设置 O_NOATIME
int fd = open(path, O_RDWR | O_DIRECT);
int flags = fcntl(fd, F_GETFL);
fcntl(fd, F_SETFL, flags | O_NOATIME);

// 方法 3: 使用 XFS 的 inode 属性
struct fsxattr fsx;
ioctl(fd, FS_IOC_FSGETXATTR, &fsx);
fsx.fsx_xflags |= FS_XFLAG_NOATIME;
ioctl(fd, FS_IOC_FSSETXATTR, &fsx);
```

### 9.2 减少文件系统元数据操作

```
1. 单一大文件: 避免创建/删除文件的操作
2. O_DIRECT: 绕过页缓存，减少 inode 操作
3. fallocate: 预分配空间，避免动态扩展的元数据更新
4. FALLOC_FL_KEEP_SIZE: 避免更新文件大小
5. 不使用 fdatasync/fsync: KVCache 场景可接受少量数据丢失
```

## 10. 性能优化

### 10.1 关键优化汇总

| 优化项 | 实现手段 | 预期效果 |
|--------|----------|----------|
| DirectIO | O_DIRECT 标志 | 绕过 OS 页缓存，减少 CPU 开销 |
| 内存对齐 | posix_memalign + buffer 池 | 满足 DirectIO 对齐要求 |
| 大文件预分配 | fallocate | 避免运行时文件扩展开销 |
| 禁用时间更新 | noatime + O_NOATIME | 减少文件元数据写入 |
| 批量 IO | preadv/pwritev | 合并多次 IO 为一次系统调用 |
| 零拷贝传输 | brpc attachment | 避免 protobuf 序列化 |
| 对齐 buffer 池 | 预分配对齐内存 | 避免频繁内存分配/释放 |
| 写入排序 | 按 offset 排序 | 优化 SSD 顺序写入性能 |

### 10.2 性能基准

| 操作 | 目标延迟 | 说明 |
|------|----------|------|
| Write (2MB, 本地 DirectIO) | < 500us | NVMe SSD 写入 |
| Read (2MB, 本地 DirectIO) | < 300us | NVMe SSD 读取 |
| Write (2MB, 远程) | < 3ms | 网络 + SSD 写入 |
| Read (2MB, 远程) | < 2ms | SSD 读取 + 网络传输 |
| BatchWrite (100 * 2MB) | < 10ms | 并行 DirectIO |

## 11. 配置项

| 配置项 | 默认值 | 说明 |
|--------|--------|------|
| `store_ssd_path` | /data/falconkv | SSD 数据根目录 |
| `store_id` | 0 | Store 唯一标识 |
| `store_capacity_gb` | 500 | Store 容量（GB） |
| `store_chunk_size` | 2097152 | KV 块大小（2MB） |
| `store_page_size` | 4096 | 页大小 |
| `store_io_threads` | 4 | IO 工作线程数 |
| `store_alignment` | 512 | DirectIO 对齐大小 |
| `store_disable_mtime` | true | 禁用文件修改时间更新 |
| `store_use_io_uring` | false | 是否使用 io_uring |
| `store_listen_port` | 8901 | Store RPC 监听端口 |
| `store_heartbeat_sec` | 5 | 心跳间隔 |
| `store_buffer_pool_size` | 64 | 对齐 buffer 池大小 |
| `store_scheduler_enabled` | true | 是否启用 Scheduler 上报 |
| `store_scheduler_uds_path` | /tmp/falconkv_scheduler.sock | Scheduler UDS 路径 |
| `store_meta_addr` | localhost:8900 | Meta 服务器地址 |
| `store_evict_interval_sec` | 60 | 驱逐扫描间隔 |
| `store_evict_high_watermark` | 0.85 | 驱逐触发水位 |
| `store_evict_low_watermark` | 0.70 | 驱逐目标水位 |
| `store_evict_grace_period_ms` | 5000 | 驱逐宽限期（Meta 删除后延迟回收本地空间的时间） |
| `store_sync_batch_size` | 256 | SyncCommit 单次最大 key 数量 |

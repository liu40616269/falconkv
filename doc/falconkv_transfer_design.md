# FalconKV Transfer 模块设计文档

## 1. 模块概述

Transfer 模块是 FalconKV 的数据传输与通信基础设施层，负责：

- **数据传输**：Client 与 Store 之间的高效数据读写
- **元数据通信**：Client 与 Meta 之间的 RPC 调用
- **连接管理**：维护与各模块的长连接和连接池
- **协议抽象**：通过接口抽象适配多种通信协议（当前实现 brpc）

### 1.1 设计参考

- **FalconFS brpc_comm_adapter**：brpc 通信适配器，提供 RPC 和数据传输能力
- **FalconFS connection_pool**：连接池管理，复用 TCP 连接

### 1.2 核心需求

| 需求 | 说明 |
|------|------|
| 零拷贝传输 | 大块 KV 数据传输避免内存拷贝 |
| 高并发 | 支持数百并发 RPC 调用 |
| 连接复用 | 避免频繁建立/断开连接 |
| 协议可插拔 | 支持未来替换为 RDMA 等协议 |

## 2. 架构设计

### 2.1 接口抽象层

```cpp
// 传输接口抽象
class TransferChannel {
public:
    virtual ~TransferChannel() = default;

    // 连接管理
    virtual Status Connect(const std::string& addr) = 0;
    virtual void Disconnect() = 0;
    virtual bool IsConnected() const = 0;

    // 同步 RPC
    virtual Status SyncCall(const RpcRequest& request,
                            RpcResponse* response) = 0;

    // 异步 RPC
    virtual Status AsyncCall(const RpcRequest& request,
                             RpcCallback callback) = 0;

    // 数据传输（零拷贝）
    virtual Status WriteData(const std::string& addr,
                             uint64_t offset,
                             const void* data,
                             uint32_t size) = 0;

    virtual Status ReadData(const std::string& addr,
                            uint64_t offset,
                            void* buffer,
                            uint32_t size) = 0;

    // 批量数据传输
    virtual Status BatchWriteData(
        const std::string& addr,
        const std::vector<DataSegment>& segments) = 0;

    virtual Status BatchReadData(
        const std::string& addr,
        const std::vector<DataSegment>& segments) = 0;
};

// 数据段描述
struct DataSegment {
    uint64_t offset;       // 文件偏移
    const void* data;      // 数据指针（写）/ 缓冲区指针（读）
    void* buffer;          // 目标缓冲区
    uint32_t size;         // 数据大小
};
```

### 2.2 brpc 实现

```cpp
class BrpcChannel : public TransferChannel {
public:
    Status Connect(const std::string& addr) override;
    void Disconnect() override;
    bool IsConnected() override;

    Status SyncCall(const RpcRequest& request,
                    RpcResponse* response) override;

    Status AsyncCall(const RpcRequest& request,
                     RpcCallback callback) override;

    Status WriteData(const std::string& addr,
                     uint64_t offset,
                     const void* data,
                     uint32_t size) override;

    Status ReadData(const std::string& addr,
                    uint64_t offset,
                    void* buffer,
                    uint32_t size) override;

    Status BatchWriteData(
        const std::string& addr,
        const std::vector<DataSegment>& segments) override;

    Status BatchReadData(
        const std::string& addr,
        const std::vector<DataSegment>& segments) override;

private:
    brpc::Channel channel_;
    std::unique_ptr<FalconKVStoreService_Stub> store_stub_;
    std::unique_ptr<FalconKVMetaService_Stub> meta_stub_;
};
```

### 2.3 Transfer 管理器

```cpp
class TransferManager {
public:
    TransferManager(const TransferConfig& config);
    ~TransferManager();

    // 获取到 Meta 的通道
    TransferChannel* GetMetaChannel();

    // 获取到指定 Store 的通道
    TransferChannel* GetStoreChannel(uint32_t store_id);

    // 批量获取多个 Store 的通道
    std::vector<TransferChannel*> GetStoreChannels(
        const std::vector<uint32_t>& store_ids);

    // 关闭所有通道
    void CloseAll();

private:
    // 创建通道（工厂模式）
    std::unique_ptr<TransferChannel> CreateChannel(
        const std::string& addr);

    TransferConfig config_;

    // Meta 通道
    std::unique_ptr<TransferChannel> meta_channel_;
    std::mutex meta_mutex_;

    // Store 通道池
    // store_id → 通道列表（可能多个连接用于并发）
    std::unordered_map<uint32_t,
                       std::vector<std::unique_ptr<TransferChannel>>>
        store_channels_;
    std::mutex store_mutex_;

    // Store 地址缓存
    // store_id → address
    std::unordered_map<uint32_t, std::string> store_addrs_;
};
```

## 3. 连接管理

### 3.1 连接池设计

```
┌────────────────────────────────────────────────────────┐
│                   TransferManager                       │
│                                                         │
│  ┌─────────────────────────────────────────────────┐   │
│  │ Meta Channel Pool                                │   │
│  │  ┌────────┐ ┌────────┐ ┌────────┐              │   │
│  │  │Channel0│ │Channel1│ │Channel2│  (N 连接)    │   │
│  │  └────────┘ └────────┘ └────────┘              │   │
│  └─────────────────────────────────────────────────┘   │
│                                                         │
│  ┌─────────────────────────────────────────────────┐   │
│  │ Store Channel Pool                               │   │
│  │  ┌─ Store 0 ──────────────────────────────────┐ │   │
│  │  │  ┌────────┐ ┌────────┐ ┌────────┐          │ │   │
│  │  │  │Channel0│ │Channel1│ │Channel2│  (M/N)   │ │   │
│  │  │  └────────┘ └────────┘ └────────┘          │ │   │
│  │  └──────────────────────────────────────────────┘ │   │
│  │  ┌─ Store 1 ──────────────────────────────────┐ │   │
│  │  │  ┌────────┐ ┌────────┐ ┌────────┐          │ │   │
│  │  │  │Channel0│ │Channel1│ │Channel2│          │ │   │
│  │  │  └────────┘ └────────┘ └────────┘          │ │   │
│  │  └──────────────────────────────────────────────┘ │   │
│  └─────────────────────────────────────────────────┘   │
└────────────────────────────────────────────────────────┘
```

### 3.2 连接池参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `meta_pool_size` | 4 | Meta 连接池大小 |
| `store_pool_size` | 4 | 每个 Store 连接池大小 |
| `connect_timeout_ms` | 3000 | 连接超时 |
| `rpc_timeout_ms` | 5000 | RPC 超时 |
| `max_retry` | 3 | 最大重试次数 |
| `idle_timeout_ms` | 30000 | 空闲连接超时回收 |

### 3.3 连接健康检查

```cpp
class ConnectionHealthChecker {
public:
    void Start() {
        thread_ = std::thread(&ConnectionHealthChecker::CheckLoop, this);
    }

    void Stop() {
        running_ = false;
        thread_.join();
    }

private:
    void CheckLoop() {
        while (running_) {
            std::this_thread::sleep_for(check_interval_);

            // 检查所有 Meta 通道
            for (auto& ch : meta_channels_) {
                if (!ch->IsConnected()) {
                    ch->Reconnect();
                }
            }

            // 检查所有 Store 通道
            for (auto& [id, channels] : store_channels_) {
                for (auto& ch : channels) {
                    if (!ch->IsConnected()) {
                        ch->Reconnect();
                    }
                }
            }
        }
    }

    std::chrono::seconds check_interval_{10};
    std::thread thread_;
    std::atomic<bool> running_{true};
};
```

## 4. Store RPC 接口定义

### 4.1 Protobuf 定义

```protobuf
// falconkv_store.proto

service FalconKVStoreService {
    // 写入数据
    rpc Write(WriteRequest) returns (WriteResponse);

    // 读取数据
    rpc Read(ReadRequest) returns (ReadResponse);

    // 批量写入
    rpc BatchWrite(BatchWriteRequest) returns (BatchWriteResponse);

    // 批量读取
    rpc BatchRead(BatchReadRequest) returns (BatchReadResponse);

    // 心跳
    rpc Ping(PingRequest) returns (PongResponse);
}

message WriteRequest {
    uint64 offset = 1;          // 文件偏移（页面对齐）
    bytes  data = 2;            // 数据内容
    uint32 size = 3;            // 数据大小
}

message WriteResponse {
    int32 status = 1;           // 0=成功
    uint32 bytes_written = 2;
}

message ReadRequest {
    uint64 offset = 1;          // 文件偏移
    uint32 size = 2;            // 读取大小
}

message ReadResponse {
    int32 status = 1;
    bytes  data = 2;            // 数据内容
    uint32 bytes_read = 3;
}

message DataSegment {
    uint64 offset = 1;
    uint32 size = 2;
    bytes  data = 3;            // 写入时携带数据
}

message BatchWriteRequest {
    repeated DataSegment segments = 1;
}

message BatchWriteResponse {
    int32 status = 1;
    repeated uint32 bytes_written = 2;
}

message BatchReadSegment {
    uint64 offset = 1;
    uint32 size = 2;
}

message BatchReadRequest {
    repeated BatchReadSegment segments = 1;
}

message BatchReadResponse {
    int32 status = 1;
    repeated bytes data_segments = 2;
    repeated uint32 bytes_read = 3;
}
```

### 4.2 零拷贝数据传输

brpc 支持 attachment 机制实现零拷贝传输：

```cpp
// 写入数据（零拷贝）
Status BrpcChannel::WriteData(const std::string& addr,
                               uint64_t offset,
                               const void* data,
                               uint32_t size) {
    FalconKVStoreService_Stub stub(&channel_);

    WriteRequest request;
    request.set_offset(offset);
    request.set_size(size);
    // 不通过 protobuf 序列化大数据，使用 attachment

    brpc::Controller cntl;
    // 使用 brpc attachment 传递大数据，避免 protobuf 序列化开销
    cntl.request_attachment().append_user_data(
        const_cast<void*>(data), size, nullptr);

    WriteResponse response;
    stub.Write(&cntl, &request, &response, nullptr);

    if (cntl.Failed()) {
        return Status::RpcError(cntl.ErrorText());
    }
    return Status::OK();
}

// 读取数据（零拷贝）
Status BrpcChannel::ReadData(const std::string& addr,
                              uint64_t offset,
                              void* buffer,
                              uint32_t size) {
    FalconKVStoreService_Stub stub(&channel_);

    ReadRequest request;
    request.set_offset(offset);
    request.set_size(size);

    brpc::Controller cntl;
    ReadResponse response;
    stub.Read(&cntl, &request, &response, nullptr);

    if (cntl.Failed()) {
        return Status::RpcError(cntl.ErrorText());
    }

    // 从 attachment 中直接拷贝到用户 buffer
    auto& attachment = cntl.response_attachment();
    size_t bytes_to_copy = std::min(
        static_cast<size_t>(size), attachment.size());
    attachment.copy_to(buffer, bytes_to_copy);

    return Status::OK();
}
```

### 4.3 批量数据传输优化

```cpp
// 批量写入优化：将多个小数据段合并为一次 RPC
Status BrpcChannel::BatchWriteData(
    const std::string& addr,
    const std::vector<DataSegment>& segments) {

    if (segments.size() == 1) {
        return WriteData(addr, segments[0].offset,
                         segments[0].data, segments[0].size);
    }

    FalconKVStoreService_Stub stub(&channel_);
    BatchWriteRequest request;

    uint32_t total_size = 0;
    for (const auto& seg : segments) {
        auto* d = request.add_segments();
        d->set_offset(seg.offset);
        d->set_size(seg.size);
        total_size += seg.size;
    }

    brpc::Controller cntl;

    // 将所有数据段合并到 attachment
    for (const auto& seg : segments) {
        cntl.request_attachment().append(
            static_cast<const char*>(seg.data), seg.size);
    }

    BatchWriteResponse response;
    stub.BatchWrite(&cntl, &request, &response, nullptr);

    if (cntl.Failed()) {
        return Status::RpcError(cntl.ErrorText());
    }
    return Status::OK();
}
```

## 5. 协议扩展设计

### 5.1 工厂模式

```cpp
// 通道工厂
class TransferChannelFactory {
public:
    static std::unique_ptr<TransferChannel> Create(
        const std::string& protocol,
        const TransferConfig& config) {

        if (protocol == "brpc") {
            return std::make_unique<BrpcChannel>(config);
        }
        // 未来可扩展：
        // else if (protocol == "rdma") {
        //     return std::make_unique<RdmaChannel>(config);
        // }
        // else if (protocol == "ucx") {
        //     return std::make_unique<UcxChannel>(config);
        // }

        return nullptr;
    }
};
```

### 5.2 配置

`meta_addr` 由 **common 区**自动传播（`common.meta_addr → transfer.meta_addr`），其余字段为 transfer 区专属。

| JSON 字段 (`transfer.*`) | 默认值 | 说明 |
|--------------------------|--------|------|
| `protocol` | brpc | 通信协议 |
| `meta_addr` | localhost:18900 | Meta 地址（由 `common.meta_addr` 传播） |
| `meta_pool_size` | 4 | Meta 连接池大小 |
| `store_pool_size` | 4 | Store 连接池大小 |
| `rpc_timeout_ms` | 5000 | RPC 超时（毫秒） |
| `connect_timeout_ms` | 3000 | 连接超时（毫秒） |
| `max_retry` | 3 | 重试次数 |

## 6. 线程模型

### 6.1 brpc 线程模型

```
┌─────────────────────────────────────────────────────┐
│ Client Process                                       │
│                                                      │
│  ┌─────────────────────────────────────────────┐    │
│  │ Application Threads                          │    │
│  │  - 调用 SyncCall / AsyncCall                │    │
│  │  - 调用 WriteData / ReadData                │    │
│  └──────────────────────┬──────────────────────┘    │
│                         │                            │
│  ┌──────────────────────┼──────────────────────┐    │
│  │ brpc Internal        │                      │    │
│  │  ┌───────────────┐   │  ┌────────────────┐  │    │
│  │  │ I/O Threads   │◀──┘  │ Worker Threads │  │    │
│  │  │ (网络收发)     │      │ (回调处理)      │  │    │
│  │  └───────────────┘      └────────────────┘  │    │
│  └──────────────────────────────────────────────┘    │
└──────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────┐
│ Store Process                                         │
│                                                       │
│  ┌──────────────────────────────────────────────┐    │
│  │ brpc Server                                   │    │
│  │  ┌────────────────┐  ┌─────────────────────┐ │    │
│  │  │ I/O Threads    │  │ Service Threads     │ │    │
│  │  │ (接收请求)      │  │ (处理 RPC + IO)     │ │    │
│  │  └────────────────┘  └─────────────────────┘ │    │
│  └──────────────────────────────────────────────┘    │
│                                                       │
│  ┌──────────────────────────────────────────────┐    │
│  │ IO Thread Pool (DirectIO)                     │    │
│  │  - 执行实际的 SSD 读写                        │    │
│  │  - 完成后填充 RPC response                    │    │
│  └──────────────────────────────────────────────┘    │
└──────────────────────────────────────────────────────┘
```

### 6.2 并发策略

```
BatchPut 场景的并发处理：

Client 线程:
  1. 发送 Alloc RPC 到 Meta (同步等待)
  2. 收到分配结果后，按 store_id 分组
  3. 对每个 Store 并行发送 Write RPC:
     - Store A: 并行写入 N 个数据段
     - Store B: 并行写入 M 个数据段
  4. 等待所有 Write 完成
  5. 发送 Commit RPC 到 Meta (同步等待)

关键：步骤 3 是并行执行的，充分利用网络带宽和 Store 的 IO 并行能力。
```

## 7. 错误处理

### 7.1 重试策略

```cpp
class RetryPolicy {
public:
    Status ExecuteWithRetry(std::function<Status()> fn) {
        for (int i = 0; i <= max_retry_; i++) {
            Status s = fn();
            if (s.OK()) return s;

            if (!IsRetriable(s)) return s;

            if (i < max_retry_) {
                // 指数退避
                auto delay = base_delay_ * (1 << i);
                std::this_thread::sleep_for(delay);
            }
        }
        return Status::MaxRetryExceeded();
    }

private:
    int max_retry_ = 3;
    std::chrono::milliseconds base_delay_{100};

    bool IsRetriable(const Status& s) {
        // 网络超时、连接断开等可重试
        return s.IsNetworkError() || s.IsTimeout();
    }
};
```

### 7.2 故障转移

```
Store 写入失败时:
1. 通知 Meta Rollback 该 key 的分配（stat → evict）
2. Meta 重新分配到其他 Store
3. 重试写入到新 Store

Store 节点不可达时:
1. Meta 标记该 Store 为 offline
2. 重新分配到其他在线 Store
3. 连接池定期尝试重连
```

## 8. 性能优化

### 8.1 关键优化点

| 优化项 | 手段 | 预期效果 |
|--------|------|----------|
| 大数据传输 | brpc attachment 零拷贝 | 避免 protobuf 序列化开销 |
| 批量 RPC | 合并多个小请求为一次 RPC | 减少网络往返 |
| 连接复用 | 连接池 | 避免连接建立开销 |
| 并行传输 | 多 Store 并行读写 | 充分利用网络带宽 |
| 异步 RPC | brpc async call | 不阻塞工作线程 |
| 请求合并 | BatchWrite/BatchRead | 减少系统调用 |

### 8.2 性能目标

| 操作 | 延迟目标 |
|------|----------|
| Meta RPC (BatchExist 100 keys) | < 2ms |
| Store Write (1 chunk, 本地) | < 1ms |
| Store Read (1 chunk, 本地) | < 0.5ms |
| Store Write (1 chunk, 远程) | < 3ms |
| Store Read (1 chunk, 远程) | < 2ms |

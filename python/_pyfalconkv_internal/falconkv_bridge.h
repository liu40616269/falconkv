#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <brpc/server.h>

#include "src/store/store_service_impl.h"

namespace falconkv {

// Bridge 层的 buffer 描述，使用原始类型，不暴露 C++ 内部结构
struct BridgeBuffer {
    void* data_ptr;    // tensor.data_ptr()
    uint32_t size;     // tensor.nbytes()
};

class FalconKVBridge {
public:
    struct Config {
        std::string config_file;         // FalconKV 配置文件路径
        size_t cache_capacity = 100000;  // Key 描述缓存容量
    };

    explicit FalconKVBridge(const Config& config);
    ~FalconKVBridge();

    FalconKVBridge(const FalconKVBridge&) = delete;
    FalconKVBridge& operator=(const FalconKVBridge&) = delete;

    // ---- 同步批量操作 ----

    // 批量查询 key 存在性，返回命中数量
    int BatchExistSync(const std::vector<std::string>& keys);

    // 批量写入（同步），返回 0 表示全部成功，非 0 表示失败
    int BatchPutSync(const std::vector<std::string>& keys,
                     const std::vector<BridgeBuffer>& buffers);

    // 批量读取（同步），返回每个 key 实际读取的字节数，<=0 表示失败
    std::vector<int32_t> BatchGetSync(const std::vector<std::string>& keys,
                                       const std::vector<BridgeBuffer>& buffers);

    // ---- Fire-and-Forget 写入 ----

    // 异步写入：Bridge 内部线程池完成后调用 callback
    // callback 签名: void(*)(void* user_data)
    // Bridge 不持有 GIL，由 C Extension 层负责 GIL 管理
    void FireAndForgetPut(const std::vector<std::string>& keys,
                          const std::vector<BridgeBuffer>& buffers,
                          void (*callback)(void*),
                          void* user_data);

    // ---- 生命周期 ----

    void Close();

private:
    // 内部持有 FalconKVStore 和 FalconKVClientImpl 实例
    std::unique_ptr<class FalconKVStore> store_;
    std::unique_ptr<class FalconKVClientImpl> impl_;

    // Store RPC server (for cross-node ACCESS_REMOTE_RPC reads)
    std::unique_ptr<StoreServiceImpl> store_service_;
    std::unique_ptr<brpc::Server> store_rpc_server_;

    // 简单线程池用于 Fire-and-Forget
    std::vector<std::thread> workers_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<std::function<void()>> task_queue_;
    std::atomic<bool> stopped_{false};

    void WorkerLoop();
    void SubmitTask(std::function<void()> task);
    void DrainQueue();
};

}  // namespace falconkv

#pragma once

#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>

#include <brpc/channel.h>

#include "falconkv_meta.pb.h"
#include "src/common/status.h"
#include "src/store/store_meta_index.h"

namespace falconkv {

/// Client used by Store to sync metadata changes to the Meta server.
/// Uses protobuf stub directly (does not link falconkv_meta module).
class MetaSyncClient {
public:
    MetaSyncClient();
    ~MetaSyncClient();

    // Non-copyable
    MetaSyncClient(const MetaSyncClient&) = delete;
    MetaSyncClient& operator=(const MetaSyncClient&) = delete;

    /// Connect to the remote Meta server. Failure does not prevent startup.
    Status Connect(const std::string& meta_addr);

    /// Sync committed key records to Meta.
    Status SyncCommit(uint32_t store_id, const std::vector<StoreKeyRecord>& records);

    /// Sync removed/evicted keys to Meta.
    Status SyncRemove(uint32_t store_id, const std::vector<std::string>& keys);

    /// Register this store with Meta.
    Status RegisterStore(uint32_t store_id, uint32_t node_id,
                         const std::string& data_file,
                         uint64_t capacity_bytes);

    /// Store registration info for reconnect + resync.
    void SetStoreInfo(uint32_t store_id, uint32_t node_id,
                      const std::string& data_file,
                      uint64_t capacity_bytes);

    /// Set pointer to local meta index for full resync (not owned).
    void SetMetaIndex(StoreMetaIndex* meta_index);

    /// Set the Store RPC address used in RegisterStore (host:port).
    void SetStoreRpcAddr(const std::string& host, uint32_t port);

    /// Start background reconnect loop. Checks every interval_sec seconds.
    void StartReconnectLoop(int interval_sec);

    /// Stop the background reconnect loop.
    void StopReconnectLoop();

    bool connected() const { return connected_.load(); }

private:
    /// Internal connect attempt (used by both initial Connect and reconnect).
    Status TryConnect();

    /// Full resync: RegisterStore + batched SyncCommit of all committed keys.
    void FullResync();

    /// Background thread function for reconnection loop.
    void ReconnectLoop(int interval_sec);

    std::unique_ptr<brpc::Channel> channel_;
    std::unique_ptr<FalconKVMetaService_Stub> stub_;
    std::atomic<bool> connected_{false};
    std::string meta_addr_;

    // Store registration info (for reconnect)
    uint32_t store_id_ = 0;
    uint32_t node_id_ = 0;
    std::string data_file_;
    uint64_t capacity_bytes_ = 0;

    // Store RPC address for RegisterStore
    std::string store_rpc_host_;
    uint32_t store_rpc_port_ = 0;

    // Local meta index pointer (not owned, for full resync)
    StoreMetaIndex* meta_index_ = nullptr;

    // Reconnect thread
    std::thread reconnect_thread_;
    std::atomic<bool> reconnect_running_{false};
    std::mutex reconnect_mutex_;
    std::condition_variable reconnect_cv_;
};

} // namespace falconkv

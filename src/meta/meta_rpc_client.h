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
#include "src/common/types.h"
#include "src/meta/meta_manager.h"

namespace falconkv {

// Client-side RPC wrapper for the remote Meta service.
// Provides the same method signatures as MetaManager for drop-in replacement
// in FalconKVClientImpl.
class MetaRpcClient {
public:
    MetaRpcClient();
    ~MetaRpcClient();

    // Non-copyable
    MetaRpcClient(const MetaRpcClient&) = delete;
    MetaRpcClient& operator=(const MetaRpcClient&) = delete;

    /// Connect to the remote Meta server at the given address (host:port).
    /// Failure does not prevent the client from being used (operations will
    /// return empty results while disconnected).
    Status Connect(const std::string& addr);

    /// Check existence of a batch of keys.  Returns records where stat == 1.
    std::vector<KeyRecord> BatchExist(const std::vector<std::string>& keys);

    /// Look up a batch of keys.  Returns records regardless of stat (but not
    /// evicted entries).
    std::vector<KeyRecord> BatchLookup(const std::vector<std::string>& keys);

    /// Start background reconnect loop. Checks every interval_sec seconds.
    void StartReconnectLoop(int interval_sec);

    /// Stop the background reconnect loop.
    void StopReconnectLoop();

    /// Query connection status.
    bool connected() const { return connected_.load(); }

private:
    // Helper: convert proto KeyDesc -> C++ KeyRecord
    static KeyRecord ProtoToKeyRecord(const KeyDesc& desc);

    /// Internal connect attempt.
    Status TryConnect();

    /// Background thread function for reconnection loop.
    void ReconnectLoop(int interval_sec);

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

} // namespace falconkv

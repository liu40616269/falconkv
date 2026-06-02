#pragma once

#include <string>
#include <memory>

#include <brpc/channel.h>

#include "falconkv_store.pb.h"
#include "src/common/status.h"

namespace falconkv {

/// Client-side RPC wrapper for the Store service.
/// Holds a brpc::Channel and FalconKVStoreService_Stub.
/// Used for remote reads only (writes go through local store).
class StoreRpcClient {
public:
    StoreRpcClient();
    ~StoreRpcClient();

    // Non-copyable
    StoreRpcClient(const StoreRpcClient&) = delete;
    StoreRpcClient& operator=(const StoreRpcClient&) = delete;

    /// Connect to a remote Store server at the given address (host:port).
    Status Connect(const std::string& addr);

    /// Whether the client is connected.
    bool IsConnected() const { return connected_; }

    /// Read data from the remote store.
    /// @param client_id  Caller's node ID (forwarded to remote store for scheduler stats).
    /// @param source_node_addr  Caller's store address (forwarded to remote store).
    Status Read(uint64_t offset, void* buffer, uint32_t size,
                uint32_t client_id = 0,
                const std::string& source_node_addr = "");

    /// Ping the remote store.
    Status Ping();

private:
    brpc::Channel channel_;
    std::unique_ptr<FalconKVStoreService_Stub> stub_;
    bool connected_ = false;
};

} // namespace falconkv

#pragma once

#include <string>
#include <unordered_map>
#include <mutex>
#include <memory>

#include "src/store/store_rpc_client.h"

namespace falconkv {

/// Connection pool for StoreRpcClient instances, cached by address.
class StoreRpcClientManager {
public:
    StoreRpcClientManager() = default;
    ~StoreRpcClientManager();

    // Non-copyable
    StoreRpcClientManager(const StoreRpcClientManager&) = delete;
    StoreRpcClientManager& operator=(const StoreRpcClientManager&) = delete;

    /// Get or create a StoreRpcClient for the given address.
    /// Returns nullptr if connection fails.
    StoreRpcClient* GetOrCreate(const std::string& addr);

    /// Close all cached clients.
    void CloseAll();

private:
    std::mutex mutex_;
    std::unordered_map<std::string, std::unique_ptr<StoreRpcClient>> clients_;
};

} // namespace falconkv

#pragma once

#include <string>
#include <memory>

#include <brpc/server.h>

#include "src/common/config.h"
#include "src/store/falconkv_store.h"
#include "src/store/store_service_impl.h"

namespace falconkv {

/// Encapsulates a standalone Store server process.
/// Owns FalconKVStore + StoreServiceImpl + brpc::Server.
class StoreServer {
public:
    explicit StoreServer(const FalconKVStore::Config& store_config,
                         const std::string& listen_addr);
    ~StoreServer();

    // Non-copyable
    StoreServer(const StoreServer&) = delete;
    StoreServer& operator=(const StoreServer&) = delete;

    /// Initialize the data file and start the brpc server.
    Status Start();

    /// Stop the server.
    void Stop();

    /// Get the underlying FalconKVStore.
    FalconKVStore* GetStore() { return &store_; }

    /// Get the address the server is listening on.
    const std::string& ListenAddr() const { return listen_addr_; }

private:
    std::string listen_addr_;
    FalconKVStore store_;
    StoreServiceImpl service_impl_;
    brpc::Server server_;
};

} // namespace falconkv

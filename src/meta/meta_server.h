#pragma once

#include <string>
#include <memory>

#include <brpc/server.h>

#include "src/common/config.h"
#include "src/meta/meta_manager.h"
#include "src/meta/meta_service_impl.h"

namespace falconkv {

// Encapsulates a standalone Meta server process.
// Owns MetaManager + MetaServiceImpl + brpc::Server.
class MetaServer {
public:
    explicit MetaServer(const MetaConfig& config);
    ~MetaServer();

    // Non-copyable
    MetaServer(const MetaServer&) = delete;
    MetaServer& operator=(const MetaServer&) = delete;

    /// Start the Meta server (blocks until server is ready).
    Status Start();

    /// Stop the Meta server.
    void Stop();

    /// Get the underlying MetaManager (for registering stores etc.).
    MetaManager* GetMetaManager() { return &meta_manager_; }

    /// Get the address the server is listening on.
    std::string ListenAddr() const { return listen_addr_; }

private:
    MetaConfig config_;
    std::string listen_addr_;
    MetaManager meta_manager_;
    MetaServiceImpl service_impl_;
    brpc::Server server_;
};

} // namespace falconkv

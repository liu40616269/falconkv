#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "src/common/config.h"
#include "src/common/status.h"
#include "src/store/store_rpc_client_manager.h"

#ifdef FALCONKV_HAS_HIXL
#include <hixl/hixl.h>
#endif

namespace falconkv {

struct RemoteReadRequest {
    uint32_t store_id = 0;
    std::string store_addr;
    uint32_t client_id = 0;
    std::string source_node_addr;
    std::vector<uint64_t> offsets;
    std::vector<uint32_t> sizes;
    std::vector<void*> buffers;
};

class RemoteReadBackend {
public:
    virtual ~RemoteReadBackend() = default;

    virtual Status BatchRead(const RemoteReadRequest& request,
                             std::vector<int32_t>& results) = 0;
};

class BrpcRemoteReadBackend : public RemoteReadBackend {
public:
    explicit BrpcRemoteReadBackend(StoreRpcClientManager* rpc_manager);

    Status BatchRead(const RemoteReadRequest& request,
                     std::vector<int32_t>& results) override;

private:
    StoreRpcClientManager* rpc_manager_ = nullptr;
};

class HixlRemoteReadBackend : public RemoteReadBackend {
public:
    HixlRemoteReadBackend(StoreRpcClientManager* rpc_manager,
                          const TransferConfig& config);
    ~HixlRemoteReadBackend() override;

    Status BatchRead(const RemoteReadRequest& request,
                     std::vector<int32_t>& results) override;

private:
#ifdef FALCONKV_HAS_HIXL
    Status EnsureHixlInitialized();
#endif

    StoreRpcClientManager* rpc_manager_ = nullptr;
    TransferConfig config_;
    BrpcRemoteReadBackend brpc_fallback_;
#ifdef FALCONKV_HAS_HIXL
    std::unique_ptr<hixl::Hixl> hixl_engine_;
    bool hixl_initialized_ = false;
#endif
};

std::unique_ptr<RemoteReadBackend> CreateRemoteReadBackend(
    StoreRpcClientManager* rpc_manager, const TransferConfig& config);

} // namespace falconkv

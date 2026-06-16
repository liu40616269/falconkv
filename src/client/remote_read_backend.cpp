#include "src/client/remote_read_backend.h"

#include <algorithm>
#include <map>

#include "src/common/logging.h"
#include "src/store/store_rpc_client.h"

namespace falconkv {

namespace {

Status ValidateRequest(const RemoteReadRequest& request) {
    const size_t n = request.offsets.size();
    if (n != request.sizes.size() || n != request.buffers.size()) {
        return Status::InvalidArg("remote read request size mismatch");
    }
    if (request.store_addr.empty()) {
        return Status::InvalidArg("remote read store address is empty");
    }
    return Status::OK();
}

#ifdef FALCONKV_HAS_HIXL
void AddIfNotEmpty(std::map<hixl::AscendString, hixl::AscendString>* options,
                   const char* key,
                   const std::string& value) {
    if (!value.empty()) {
        (*options)[key] = value.c_str();
    }
}

std::string BuildProtocolDescConfig(const std::string& protocol_desc) {
    if (protocol_desc.empty()) {
        return "";
    }
    return std::string("{\"comm_resource_config.protocol_desc\":\"") +
           protocol_desc + "\"}";
}

std::map<hixl::AscendString, hixl::AscendString> BuildHixlOptions(
    const TransferConfig& config) {
    std::map<hixl::AscendString, hixl::AscendString> options;
    AddIfNotEmpty(&options, hixl::OPTION_LOCAL_COMM_RES,
                  config.hixl_local_comm_res);
    if (!config.hixl_global_resource_config.empty()) {
        AddIfNotEmpty(&options, hixl::OPTION_GLOBAL_RESOURCE_CONFIG,
                      config.hixl_global_resource_config);
    } else {
        AddIfNotEmpty(&options, hixl::OPTION_GLOBAL_RESOURCE_CONFIG,
                      BuildProtocolDescConfig(config.hixl_protocol_desc));
    }
    AddIfNotEmpty(&options, hixl::OPTION_BUFFER_POOL,
                  config.hixl_buffer_pool);
    if (config.hixl_rdma_traffic_class >= 0) {
        AddIfNotEmpty(&options, hixl::OPTION_RDMA_TRAFFIC_CLASS,
                      std::to_string(config.hixl_rdma_traffic_class));
    }
    if (config.hixl_rdma_service_level >= 0) {
        AddIfNotEmpty(&options, hixl::OPTION_RDMA_SERVICE_LEVEL,
                      std::to_string(config.hixl_rdma_service_level));
    }
    return options;
}
#endif

} // namespace

BrpcRemoteReadBackend::BrpcRemoteReadBackend(StoreRpcClientManager* rpc_manager)
    : rpc_manager_(rpc_manager) {}

Status BrpcRemoteReadBackend::BatchRead(const RemoteReadRequest& request,
                                        std::vector<int32_t>& results) {
    Status valid = ValidateRequest(request);
    if (!valid.ok()) {
        results.assign(request.offsets.size(), -1);
        return valid;
    }
    if (!rpc_manager_) {
        results.assign(request.offsets.size(), -1);
        return Status::InvalidArg("StoreRpcClientManager is null");
    }

    StoreRpcClient* rpc = rpc_manager_->GetOrCreate(request.store_addr);
    if (!rpc) {
        results.assign(request.offsets.size(), -1);
        return Status::RpcError("failed to create StoreRpcClient for " +
                                request.store_addr);
    }

    return rpc->BatchRead(request.offsets, request.sizes, request.buffers,
                          results);
}

HixlRemoteReadBackend::HixlRemoteReadBackend(StoreRpcClientManager* rpc_manager,
                                             const TransferConfig& config)
    : rpc_manager_(rpc_manager),
      config_(config),
      brpc_fallback_(rpc_manager) {}

HixlRemoteReadBackend::~HixlRemoteReadBackend() {
#ifdef FALCONKV_HAS_HIXL
    if (hixl_engine_) {
        hixl_engine_->Finalize();
    }
#endif
}

#ifdef FALCONKV_HAS_HIXL
Status HixlRemoteReadBackend::EnsureHixlInitialized() {
    if (hixl_initialized_) {
        return Status::OK();
    }
    if (config_.hixl_local_engine.empty()) {
        return Status::InvalidArg("transfer.hixl_local_engine is empty");
    }

    hixl_engine_ = std::make_unique<hixl::Hixl>();
    std::map<hixl::AscendString, hixl::AscendString> options =
        BuildHixlOptions(config_);
    hixl::Status ret = hixl_engine_->Initialize(config_.hixl_local_engine.c_str(),
                                                options);
    if (ret != hixl::SUCCESS) {
        hixl_engine_.reset();
        return Status::RpcError("HIXL Initialize failed: " +
                                std::to_string(ret));
    }
    hixl_initialized_ = true;
    return Status::OK();
}
#endif

Status HixlRemoteReadBackend::BatchRead(const RemoteReadRequest& request,
                                        std::vector<int32_t>& results) {
    Status valid = ValidateRequest(request);
    if (!valid.ok()) {
        results.assign(request.offsets.size(), -1);
        return valid;
    }

#if defined(FALCONKV_HAS_HIXL)
    if (!rpc_manager_) {
        results.assign(request.offsets.size(), -1);
        return Status::InvalidArg("StoreRpcClientManager is null");
    }

    Status init = EnsureHixlInitialized();
    if (!init.ok()) {
        if (config_.hixl_fallback_to_brpc) {
            LOG(WARNING) << "[HixlRemoteReadBackend] " << init.ToString()
                         << "; using brpc fallback";
            return brpc_fallback_.BatchRead(request, results);
        }
        results.assign(request.offsets.size(), -1);
        return init;
    }

    StoreRpcClient* rpc = rpc_manager_->GetOrCreate(request.store_addr);
    if (!rpc) {
        results.assign(request.offsets.size(), -1);
        return Status::RpcError("failed to create StoreRpcClient for " +
                                request.store_addr);
    }

    HixlBatchReadResponse prepare_response;
    Status prepare = rpc->PrepareHixlBatchRead(
        request.offsets, request.sizes, request.client_id,
        request.source_node_addr, &prepare_response);
    if (!prepare.ok()) {
        if (config_.hixl_fallback_to_brpc) {
            LOG(WARNING) << "[HixlRemoteReadBackend] prepare failed: "
                         << prepare.ToString() << "; using brpc fallback";
            return brpc_fallback_.BatchRead(request, results);
        }
        results.assign(request.offsets.size(), -1);
        return prepare;
    }

    if (prepare_response.segments_size() !=
        static_cast<int>(request.offsets.size())) {
        rpc->ReleaseHixlRead(prepare_response.token());
        results.assign(request.offsets.size(), -1);
        return Status::IoError("HIXL prepare segment count mismatch");
    }

    std::vector<hixl::MemHandle> local_handles;
    std::vector<hixl::TransferOpDesc> descs;
    local_handles.reserve(request.buffers.size());
    descs.reserve(request.buffers.size());

    for (size_t i = 0; i < request.buffers.size(); ++i) {
        const auto& seg = prepare_response.segments(static_cast<int>(i));
        if (seg.status() != 0 || seg.size() == 0) {
            results.assign(request.offsets.size(), -1);
            rpc->ReleaseHixlRead(prepare_response.token());
            return Status::IoError("HIXL prepare returned invalid segment");
        }

        hixl::MemDesc mem{reinterpret_cast<uintptr_t>(request.buffers[i]),
                          request.sizes[i]};
        hixl::MemHandle handle = nullptr;
        hixl::Status ret = hixl_engine_->RegisterMem(mem, hixl::MEM_HOST,
                                                     handle);
        if (ret != hixl::SUCCESS) {
            for (auto h : local_handles) {
                hixl_engine_->DeregisterMem(h);
            }
            rpc->ReleaseHixlRead(prepare_response.token());
            results.assign(request.offsets.size(), -1);
            return Status::RpcError("HIXL RegisterMem failed: " +
                                    std::to_string(ret));
        }
        local_handles.push_back(handle);
        descs.push_back({reinterpret_cast<uintptr_t>(request.buffers[i]),
                         static_cast<uintptr_t>(seg.remote_addr()),
                         seg.size()});
    }

    hixl::Status conn = hixl_engine_->Connect(
        prepare_response.remote_engine().c_str(), config_.hixl_timeout_ms);
    if (conn != hixl::SUCCESS && conn != hixl::ALREADY_CONNECTED) {
        for (auto h : local_handles) {
            hixl_engine_->DeregisterMem(h);
        }
        rpc->ReleaseHixlRead(prepare_response.token());
        results.assign(request.offsets.size(), -1);
        return Status::RpcError("HIXL Connect failed: " +
                                std::to_string(conn));
    }

    hixl::Status ret = hixl_engine_->TransferSync(
        prepare_response.remote_engine().c_str(), hixl::READ, descs,
        config_.hixl_timeout_ms);

    for (auto h : local_handles) {
        hixl_engine_->DeregisterMem(h);
    }
    Status release = rpc->ReleaseHixlRead(prepare_response.token());
    if (!release.ok()) {
        LOG(WARNING) << "[HixlRemoteReadBackend] remote release failed: "
                     << release.ToString();
    }

    if (ret != hixl::SUCCESS) {
        results.assign(request.offsets.size(), -1);
        return Status::RpcError("HIXL TransferSync READ failed: " +
                                std::to_string(ret));
    }

    results.resize(request.sizes.size());
    for (size_t i = 0; i < request.sizes.size(); ++i) {
        results[i] = static_cast<int32_t>(request.sizes[i]);
    }
    return Status::OK();
#else
    LOG(WARNING) << "[HixlRemoteReadBackend] FalconKV was built without HIXL; "
                 << "using brpc fallback for store " << request.store_id;

    if (config_.hixl_fallback_to_brpc) {
        return brpc_fallback_.BatchRead(request, results);
    }

    results.assign(request.offsets.size(), -1);
    return Status::NotSupported("HIXL remote read backend is not available");
#endif
}

std::unique_ptr<RemoteReadBackend> CreateRemoteReadBackend(
    StoreRpcClientManager* rpc_manager, const TransferConfig& config) {
    if (config.data_protocol == "hixl" || config.protocol == "hixl") {
        return std::make_unique<HixlRemoteReadBackend>(rpc_manager, config);
    }
    return std::make_unique<BrpcRemoteReadBackend>(rpc_manager);
}

} // namespace falconkv

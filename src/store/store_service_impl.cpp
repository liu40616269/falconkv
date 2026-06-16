#include "src/store/store_service_impl.h"
#include "src/store/store_meta_index.h"

#include <brpc/closure_guard.h>
#include <brpc/controller.h>

#include <cstring>
#include <map>
#include <sstream>
#include <unordered_map>
#include "src/common/time_util.h"

namespace falconkv {

namespace {

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

std::map<hixl::AscendString, hixl::AscendString> BuildStoreHixlOptions(
    const FalconKVStore::Config& config) {
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

StoreServiceImpl::StoreServiceImpl(FalconKVStore* store)
    : store_(store) {}

StoreServiceImpl::~StoreServiceImpl() {
#ifdef FALCONKV_HAS_HIXL
    std::lock_guard<std::mutex> lock(hixl_mutex_);
    for (auto& item : hixl_leases_) {
        for (auto handle : item.second.handles) {
            if (hixl_engine_) {
                hixl_engine_->DeregisterMem(handle);
            }
        }
        for (void* buf : item.second.buffers) {
            AlignedAllocator::Free(buf);
        }
    }
    hixl_leases_.clear();
    if (hixl_engine_) {
        hixl_engine_->Finalize();
    }
#endif
}

#ifdef FALCONKV_HAS_HIXL
Status StoreServiceImpl::EnsureHixlInitialized() {
    if (hixl_initialized_) {
        return Status::OK();
    }
    if (!store_ || store_->hixl_engine_addr().empty()) {
        return Status::InvalidArg("store hixl_engine_addr is empty");
    }

    hixl_engine_ = std::make_unique<hixl::Hixl>();
    std::map<hixl::AscendString, hixl::AscendString> options =
        BuildStoreHixlOptions(store_->config());
    hixl::Status ret = hixl_engine_->Initialize(store_->hixl_engine_addr().c_str(),
                                                options);
    if (ret != hixl::SUCCESS) {
        hixl_engine_.reset();
        return Status::RpcError("HIXL Initialize failed: " +
                                std::to_string(ret));
    }

    hixl_initialized_ = true;
    LOG(INFO) << "[StoreServiceImpl] HIXL initialized at "
              << store_->hixl_engine_addr();
    return Status::OK();
}
#endif

// -----------------------------------------------------------------
// Read (offset-based, for remote client backward compatibility)
// -----------------------------------------------------------------
void StoreServiceImpl::Read(::google::protobuf::RpcController* controller,
                             const ReadRequest* request,
                             ReadResponse* response,
                             ::google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);

    uint64_t offset = request->offset();
    uint32_t size = request->size();

    // Allocate aligned buffer for DirectIO read
    void* buffer = AlignedAllocator::Allocate(512, size);
    if (!buffer) {
        response->set_status(-1);
        return;
    }

    uint64_t request_ts_ns = GetCurrentTimeNs();
    Status s = store_->Read(offset, buffer, size);
    uint64_t done_ts_ns = GetCurrentTimeNs();

    if (!s.ok()) {
        AlignedAllocator::Free(buffer);
        response->set_status(static_cast<int>(s.code()));
        return;
    }

    // Report to Scheduler (Store perspective: NET_RX_READ)
    if (store_->scheduler_proxy()) {
        store_->scheduler_proxy()->StoreReportIOAsync(
            store_->store_id(),
            3,  // NET_RX_READ
            request->client_id(),
            size,
            request_ts_ns,
            done_ts_ns,
            request->source_node_addr());
    }

    response->set_status(0);
    response->set_bytes_read(size);

    // Zero-copy: transfer buffer ownership to brpc attachment.
    // The deleter frees the buffer after brpc finishes sending.
    auto* cntl = static_cast<brpc::Controller*>(controller);
    cntl->response_attachment().append_user_data(
        buffer, size, AlignedAllocator::Free);
}

// -----------------------------------------------------------------
// BatchRead (offset-based)
// -----------------------------------------------------------------
void StoreServiceImpl::BatchRead(::google::protobuf::RpcController* controller,
                                  const BatchReadRequest* request,
                                  BatchReadResponse* response,
                                  ::google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);

    std::vector<ReadItem> items;
    std::vector<std::unique_ptr<void, void(*)(void*)>> buffers;

    items.reserve(request->segments_size());

    for (int i = 0; i < request->segments_size(); ++i) {
        const auto& seg = request->segments(i);
        uint32_t size = seg.size();

        void* buf = AlignedAllocator::Allocate(512, size);
        if (!buf) {
            response->set_status(-1);
            return;
        }
        buffers.emplace_back(buf, &AlignedAllocator::Free);
        items.push_back({seg.offset(), buf, size});
    }

    uint64_t total_io_size = 0;
    for (const auto& item : items) total_io_size += item.size;

    uint64_t request_ts_ns = GetCurrentTimeNs();
    Status s = store_->BatchRead(items);
    uint64_t done_ts_ns = GetCurrentTimeNs();

    // Report to Scheduler (Store perspective: NET_RX_READ)
    if (store_->scheduler_proxy() && s.ok()) {
        store_->scheduler_proxy()->StoreReportIOAsync(
            store_->store_id(),
            3,  // NET_RX_READ
            0,  // client_id not available in BatchReadRequest
            total_io_size,
            request_ts_ns,
            done_ts_ns,
            "");  // source_node_addr not available in BatchReadRequest
    }

    response->set_status(s.ok() ? 0 : static_cast<int>(s.code()));
    if (!s.ok()) {
        for (size_t i = 0; i < items.size(); ++i) {
            response->add_bytes_read(0);
        }
        return;
    }

    // Fill bytes_read metadata.
    for (size_t i = 0; i < items.size(); ++i) {
        response->add_bytes_read(items[i].size);
    }

    // Zero-copy: transfer each buffer's ownership to brpc attachment.
    // Layout: [seg0_data][seg1_data]...[segN_data] concatenated sequentially.
    // Client uses bytes_read[] to locate each segment's offset.
    auto* cntl = static_cast<brpc::Controller*>(controller);
    for (size_t i = 0; i < items.size(); ++i) {
        cntl->response_attachment().append_user_data(
            buffers[i].release(), items[i].size, AlignedAllocator::Free);
    }
}

// -----------------------------------------------------------------
// PrepareHixlBatchRead
// -----------------------------------------------------------------
void StoreServiceImpl::PrepareHixlBatchRead(::google::protobuf::RpcController*,
                                             const HixlBatchReadRequest* request,
                                             HixlBatchReadResponse* response,
                                             ::google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);

#ifndef FALCONKV_HAS_HIXL
    response->set_status(static_cast<int>(Status::kNotSupported));
    return;
#else
    {
        std::lock_guard<std::mutex> lock(hixl_mutex_);
        Status init = EnsureHixlInitialized();
        if (!init.ok()) {
            response->set_status(static_cast<int>(init.code()));
            return;
        }
    }

    std::vector<ReadItem> items;
    HixlReadLease lease;
    items.reserve(request->segments_size());
    lease.buffers.reserve(request->segments_size());
    lease.handles.reserve(request->segments_size());

    for (int i = 0; i < request->segments_size(); ++i) {
        const auto& seg = request->segments(i);
        void* buf = AlignedAllocator::Allocate(512, seg.size());
        if (!buf) {
            response->set_status(-1);
            for (void* allocated : lease.buffers) {
                AlignedAllocator::Free(allocated);
            }
            return;
        }
        lease.buffers.push_back(buf);
        items.push_back({seg.offset(), buf, seg.size()});
    }

    uint64_t total_io_size = 0;
    for (const auto& item : items) total_io_size += item.size;

    uint64_t request_ts_ns = GetCurrentTimeNs();
    Status s = store_->BatchRead(items);
    uint64_t done_ts_ns = GetCurrentTimeNs();

    if (store_->scheduler_proxy() && s.ok()) {
        store_->scheduler_proxy()->StoreReportIOAsync(
            store_->store_id(),
            3,  // NET_RX_READ
            request->client_id(),
            total_io_size,
            request_ts_ns,
            done_ts_ns,
            request->source_node_addr());
    }

    if (!s.ok()) {
        response->set_status(static_cast<int>(s.code()));
        for (void* buf : lease.buffers) {
            AlignedAllocator::Free(buf);
        }
        return;
    }

    {
        std::lock_guard<std::mutex> lock(hixl_mutex_);
        for (const auto& item : items) {
            hixl::MemDesc desc{reinterpret_cast<uintptr_t>(item.buffer),
                               item.size};
            hixl::MemHandle handle = nullptr;
            hixl::Status ret = hixl_engine_->RegisterMem(desc, hixl::MEM_HOST,
                                                         handle);
            if (ret != hixl::SUCCESS) {
                for (auto registered : lease.handles) {
                    hixl_engine_->DeregisterMem(registered);
                }
                for (void* buf : lease.buffers) {
                    AlignedAllocator::Free(buf);
                }
                response->set_status(static_cast<int>(Status::kRpcError));
                return;
            }
            lease.handles.push_back(handle);
        }

        std::ostringstream token;
        token << store_->store_id() << "-" << hixl_next_token_.fetch_add(1);
        response->set_status(0);
        response->set_token(token.str());
        response->set_remote_engine(store_->hixl_engine_addr());

        for (const auto& item : items) {
            auto* seg = response->add_segments();
            seg->set_remote_addr(reinterpret_cast<uintptr_t>(item.buffer));
            seg->set_size(item.size);
            seg->set_status(0);
        }

        hixl_leases_.emplace(token.str(), std::move(lease));
    }
#endif
}

// -----------------------------------------------------------------
// ReleaseHixlRead
// -----------------------------------------------------------------
void StoreServiceImpl::ReleaseHixlRead(::google::protobuf::RpcController*,
                                        const HixlReleaseRequest* request,
                                        HixlReleaseResponse* response,
                                        ::google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);

#ifndef FALCONKV_HAS_HIXL
    response->set_status(0);
    return;
#else
    HixlReadLease lease;
    {
        std::lock_guard<std::mutex> lock(hixl_mutex_);
        auto it = hixl_leases_.find(request->token());
        if (it == hixl_leases_.end()) {
            response->set_status(static_cast<int>(Status::kNotFound));
            return;
        }
        lease = std::move(it->second);
        hixl_leases_.erase(it);
    }

    {
        std::lock_guard<std::mutex> lock(hixl_mutex_);
        if (hixl_engine_) {
            for (auto handle : lease.handles) {
                hixl_engine_->DeregisterMem(handle);
            }
        }
    }
    for (void* buf : lease.buffers) {
        AlignedAllocator::Free(buf);
    }
    response->set_status(0);
#endif
}

// -----------------------------------------------------------------
// GetByKey (key-based read)
// -----------------------------------------------------------------
void StoreServiceImpl::GetByKey(::google::protobuf::RpcController*,
                                 const GetByKeyRequest* request,
                                 GetByKeyResponse* response,
                                 ::google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);

    const std::string& key = request->key();

    // First look up the key to get its actual size.
    std::vector<StoreKeyRecord> hits;
    std::vector<std::string> misses;
    store_->BatchContains({key}, hits, misses);
    if (hits.empty()) {
        response->set_status(static_cast<int>(Status::kNotFound));
        return;
    }

    uint32_t data_size = hits[0].size;
    void* buffer = AlignedAllocator::Allocate(512, data_size);
    if (!buffer) {
        response->set_status(-1);
        return;
    }

    uint64_t request_ts_ns = GetCurrentTimeNs();
    auto result = store_->Get(key, buffer, data_size);
    uint64_t done_ts_ns = GetCurrentTimeNs();

    if (!result.status.ok()) {
        AlignedAllocator::Free(buffer);
        response->set_status(static_cast<int>(result.status.code()));
        return;
    }

    // Report to Scheduler (Store perspective: NET_RX_READ)
    if (store_->scheduler_proxy()) {
        store_->scheduler_proxy()->StoreReportIOAsync(
            store_->store_id(),
            3,  // NET_RX_READ
            request->client_id(),
            result.size,
            request_ts_ns,
            done_ts_ns,
            request->source_node_addr());
    }

    response->set_status(0);
    response->set_data(buffer, result.size);
    response->set_size(result.size);
    AlignedAllocator::Free(buffer);
}

// -----------------------------------------------------------------
// BatchGetByKey (key-based batch read)
// -----------------------------------------------------------------
void StoreServiceImpl::BatchGetByKey(::google::protobuf::RpcController*,
                                      const BatchGetByKeyRequest* request,
                                      BatchGetByKeyResponse* response,
                                      ::google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);

    // Batch look up all keys to get their actual sizes.
    std::vector<std::string> keys;
    keys.reserve(request->keys_size());
    for (int i = 0; i < request->keys_size(); ++i) {
        keys.push_back(request->keys(i));
    }

    std::vector<StoreKeyRecord> hits;
    std::vector<std::string> misses;
    store_->BatchContains(keys, hits, misses);

    // Build a set of found keys with their sizes for quick lookup.
    std::unordered_map<std::string, uint32_t> key_sizes;
    for (const auto& rec : hits) {
        key_sizes[rec.key] = rec.size;
    }

    bool all_ok = true;

    for (int i = 0; i < request->keys_size(); ++i) {
        const std::string& key = request->keys(i);

        auto it = key_sizes.find(key);
        if (it == key_sizes.end()) {
            response->add_data_segments();
            response->add_sizes(0);
            response->add_statuses(static_cast<int>(Status::kNotFound));
            all_ok = false;
            continue;
        }

        uint32_t data_size = it->second;
        void* buffer = AlignedAllocator::Allocate(512, data_size);
        if (!buffer) {
            response->add_data_segments();
            response->add_sizes(0);
            response->add_statuses(-1);
            all_ok = false;
            continue;
        }

        uint64_t request_ts_ns = GetCurrentTimeNs();
        auto result = store_->Get(key, buffer, data_size);
        uint64_t done_ts_ns = GetCurrentTimeNs();

        if (!result.status.ok()) {
            AlignedAllocator::Free(buffer);
            response->add_data_segments();
            response->add_sizes(0);
            response->add_statuses(static_cast<int>(result.status.code()));
            all_ok = false;
            continue;
        }

        // Report to Scheduler (Store perspective: NET_RX_READ)
        if (store_->scheduler_proxy()) {
            store_->scheduler_proxy()->StoreReportIOAsync(
                store_->store_id(),
                3,  // NET_RX_READ
                request->client_id(),
                result.size,
                request_ts_ns,
                done_ts_ns,
                request->source_node_addr());
        }

        response->add_data_segments(buffer, result.size);
        response->add_sizes(result.size);
        response->add_statuses(0);
        AlignedAllocator::Free(buffer);
    }

    response->set_status(all_ok ? 0 : 1);
}

// -----------------------------------------------------------------
// Ping
// -----------------------------------------------------------------
void StoreServiceImpl::Ping(::google::protobuf::RpcController*,
                             const PingRequest* request,
                             PongResponse* response,
                             ::google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);

    response->set_status(0);
    response->set_timestamp_ns(request->timestamp_ns());
}

} // namespace falconkv

#include "client/falconkv_client_impl.h"

#include <algorithm>
#include <unordered_map>

#include "src/common/logging.h"

namespace falconkv {

FalconKVClientImpl::FalconKVClientImpl(const Config& config)
    : config_(config),
      key_desc_cache_(config.cache_capacity) {
    // Load config
    FalconKVConfig cfg;
    if (!config_.config_file.empty()) {
        cfg = ConfigLoader::LoadFromFile(config_.config_file);
    }

    // Read node_id from config (client section takes precedence)
    node_id_ = cfg.client.node_id;
    if (node_id_ == 0) {
        node_id_ = cfg.store.node_id;
    }

    // Compute local store address for remote-read source identification
    local_store_addr_ = cfg.store.store_rpc_host + ":"
                        + std::to_string(cfg.store.listen_port);

    // Connect to remote Meta server via RPC
    Status meta_status = meta_client_.Connect(cfg.transfer.meta_addr);
    if (!meta_status.ok()) {
        LOG(WARNING) << "[FalconKVClient] Failed to connect to Meta server: "
                     << meta_status.ToString()
                     << " (will retry via background reconnect loop)";
    }

    // Initialize SchedulerProxy if enabled.
    // Prefer settings from the loaded JSON config when a config file is given.
    bool sched_enabled = config_.scheduler_enabled;
    std::string sched_uds = config_.scheduler_uds_path;
    if (!config_.config_file.empty()) {
        sched_enabled = cfg.common.scheduler_enabled;
        if (!cfg.common.scheduler_uds_path.empty()) {
            sched_uds = cfg.common.scheduler_uds_path;
        }
    }
    scheduler_enabled_ = sched_enabled;
    if (scheduler_enabled_) {
        scheduler_proxy_ = std::make_unique<SchedulerProxy>(sched_uds,
                                                             cfg.client.scheduler_rpc_timeout_us / 1000,
                                                             cfg.client.max_consecutive_failures,
                                                             cfg.client.reconnect_interval_sec);
    }

    // Start background reconnect loop for Meta RPC client.
    meta_client_.StartReconnectLoop(5);
}

FalconKVClientImpl::~FalconKVClientImpl() {
    Close();
}

// -----------------------------------------------------------------
// Unified read dispatch
// -----------------------------------------------------------------
Status FalconKVClientImpl::DoRead(const KeyDescriptor& desc,
                                   void* buffer, uint32_t size) {
    switch (desc.access_type) {
        case AccessType::ACCESS_LOCAL_DIRECT:
            if (local_store_) {
                auto result = local_store_->Get(desc.key, buffer, size);
                return result.status;
            }
            LOG(ERROR) << "[FalconKVClient] DoRead: local store not set for ACCESS_LOCAL_DIRECT, key="
                       << desc.key;
            return Status::IoError("local store not set for ACCESS_LOCAL_DIRECT");

        case AccessType::ACCESS_NODE_DIRECT:
            return node_accessor_.Read(desc.store_id, desc.offset,
                                        buffer, size);

        case AccessType::ACCESS_REMOTE_RPC: {
            StoreRpcClient* rpc = GetStoreRpcClient(desc.store_id);
            if (!rpc) {
                LOG(ERROR) << "[FalconKVClient] DoRead: no RPC client for store "
                           << desc.store_id << ", key=" << desc.key;
                return Status::RpcError(
                    "no RPC client for store " + std::to_string(desc.store_id));
            }
            return rpc->Read(desc.offset, buffer, size,
                             node_id_, local_store_addr_);
        }

        default:
            LOG(ERROR) << "[FalconKVClient] DoRead: unknown access type="
                       << static_cast<int>(desc.access_type) << ", key=" << desc.key;
            return Status::NotSupported("unknown access type");
    }
}

// -----------------------------------------------------------------
// GetStoreRpcClient
// -----------------------------------------------------------------
StoreRpcClient* FalconKVClientImpl::GetStoreRpcClient(uint32_t store_id) {
    auto it = store_addr_map_.find(store_id);
    if (it == store_addr_map_.end()) {
        return nullptr;
    }
    return store_rpc_mgr_.GetOrCreate(it->second);
}

// -----------------------------------------------------------------
// Scheduler helpers
// -----------------------------------------------------------------
int FalconKVClientImpl::ChannelForAccessType(AccessType type) {
    switch (type) {
        case AccessType::ACCESS_LOCAL_DIRECT:   return 0; // LOCAL_SSD_READ
        case AccessType::ACCESS_NODE_DIRECT:    return 0; // LOCAL_SSD_READ
        case AccessType::ACCESS_REMOTE_RPC:     return 2; // NET_TX_READ
        default: return 0;
    }
}

std::string FalconKVClientImpl::RemoteAddrForDesc(const KeyDescriptor& desc) {
    if (desc.access_type == AccessType::ACCESS_REMOTE_RPC) {
        if (!desc.store_addr.empty()) return desc.store_addr;
        auto it = store_addr_map_.find(desc.store_id);
        if (it != store_addr_map_.end()) return it->second;
    }
    return "";
}

// -----------------------------------------------------------------
// BatchExist
// -----------------------------------------------------------------
int FalconKVClientImpl::BatchExist(const std::vector<std::string>& keys,
                                    std::vector<KeyDescriptor>& hit_descs) {
    hit_descs.clear();

    if (keys.empty()) {
        return 0;
    }

    // Step 1: Check local Store metadata directly (no cache, to avoid stale
    // entries from evicted data).
    std::vector<StoreKeyRecord> store_hits;
    std::vector<std::string> missing_keys;
    if (local_store_) {
        local_store_->BatchContains(keys, store_hits, missing_keys);

        for (const auto& rec : store_hits) {
            KeyDescriptor desc(rec.key);
            desc.store_id = local_store_->store_id();
            desc.offset = rec.offset;
            desc.size = rec.size;
            desc.access_type = AccessType::ACCESS_LOCAL_DIRECT;
            hit_descs.push_back(desc);
            key_desc_cache_.Insert(rec.key, desc);
        }
    } else {
        missing_keys = keys;
    }

    // Step 2: For keys still missing, query remote Meta
    if (!missing_keys.empty()) {
        auto records = meta_client_.BatchExist(missing_keys);
        for (size_t i = 0; i < records.size(); ++i) {
            if (!records[i].key.empty() && records[i].stat == 1) {
                KeyDescriptor desc(records[i].key);
                desc.store_id = records[i].store_id;
                desc.offset = records[i].offset;
                desc.size = records[i].size;
                desc.access_time_ms = records[i].access_time_ms;
                // Determine access type: same node -> NODE_DIRECT, else REMOTE_RPC
                if (records[i].node_id != 0 && records[i].node_id == node_id_) {
                    desc.access_type = AccessType::ACCESS_NODE_DIRECT;
                    // Register the store's data file with NodeLocalAccessor
                    if (!records[i].data_file.empty()) {
                        node_accessor_.RegisterStoreFile(
                            records[i].store_id, records[i].data_file);
                    }
                } else {
                    desc.access_type = AccessType::ACCESS_REMOTE_RPC;
                }
                // Populate store_addr_map_ and desc.store_addr for RPC routing
                if (!records[i].store_addr.empty()) {
                    store_addr_map_[records[i].store_id] = records[i].store_addr;
                    desc.store_addr = records[i].store_addr;
                }
                hit_descs.push_back(desc);
                key_desc_cache_.Insert(records[i].key, desc);
            }
        }
    }

    return static_cast<int>(hit_descs.size());
}

// -----------------------------------------------------------------
// BatchPut
// -----------------------------------------------------------------
std::vector<Status> FalconKVClientImpl::BatchPut(
    const std::vector<std::string>& keys,
    const std::vector<BufferInfo>& buffers) {

    if (keys.size() != buffers.size()) {
        LOG(ERROR) << "[FalconKVClient] BatchPut: keys.size()=" << keys.size()
                   << " != buffers.size()=" << buffers.size();
        return {Status::InvalidArg("keys and buffers size mismatch")};
    }

    if (!local_store_) {
        LOG(ERROR) << "[FalconKVClient] BatchPut: local store not set";
        return {Status::IoError("local store not set")};
    }

    // Build data_ptrs and sizes for Store.BatchPut
    std::vector<const void*> data_ptrs;
    std::vector<uint32_t> sizes;
    data_ptrs.reserve(keys.size());
    sizes.reserve(keys.size());

    uint64_t total_io_size = 0;
    for (size_t i = 0; i < keys.size(); ++i) {
        data_ptrs.push_back(buffers[i].data_ptr);
        sizes.push_back(buffers[i].size);
        total_io_size += buffers[i].size;
    }

    // Scheduler: RequestIO for write
    uint64_t request_ts = GetCurrentTimeNs();
    IOResponseData io_resp;
    if (scheduler_enabled_) {
        IORequestData io_req;
        io_req.io_channel = 1;  // LOCAL_SSD_WRITE
        io_req.store_id = local_store_->store_id();
        io_req.io_size = total_io_size;
        io_req.request_ts_ns = request_ts;
        io_resp = scheduler_proxy_->RequestIO(io_req);
    }

    // Delegate to local store BatchPut
    uint64_t start_ts = GetCurrentTimeNs();
    auto results = local_store_->BatchPut(keys, data_ptrs, sizes);
    uint64_t done_ts = GetCurrentTimeNs();

    // Scheduler: ReportIOCompletion
    if (scheduler_enabled_) {
        IOCompletionData report;
        report.ticket = io_resp.ticket;
        report.io_start_ts_ns = start_ts;
        report.io_done_ts_ns = done_ts;
        report.io_size = total_io_size;
        report.io_channel = 1;  // LOCAL_SSD_WRITE
        report.io_status = 0;
        report.store_id = local_store_->store_id();
        for (const auto& r : results) {
            if (!r.status.ok()) { report.io_status = -1; break; }
        }
        scheduler_proxy_->ReportIOCompletion(report);
    }

    // Convert results and update KeyDescCache
    std::vector<Status> statuses(keys.size());
    for (size_t i = 0; i < results.size(); ++i) {
        statuses[i] = results[i].status;

        if (results[i].status.ok()) {
            KeyDescriptor desc(keys[i]);
            desc.store_id = local_store_->store_id();
            desc.offset = results[i].offset;
            desc.size = buffers[i].size;
            desc.access_type = AccessType::ACCESS_LOCAL_DIRECT;
            key_desc_cache_.Insert(keys[i], desc);
        }
    }

    return statuses;
}

// -----------------------------------------------------------------
// BatchGet
// -----------------------------------------------------------------
std::vector<int32_t> FalconKVClientImpl::BatchGet(
    const std::vector<KeyDescriptor>& key_descs,
    const std::vector<BufferInfo>& buffers) {

    if (key_descs.size() != buffers.size()) {
        LOG(ERROR) << "[FalconKVClient] BatchGet: key_descs.size()="
                   << key_descs.size() << " != buffers.size()=" << buffers.size();
        return std::vector<int32_t>(key_descs.size(), -1);
    }

    std::vector<int32_t> bytes_read(key_descs.size(), 0);

    for (size_t i = 0; i < key_descs.size(); ++i) {
        const auto& desc = key_descs[i];
        const auto& buf = buffers[i];

        LOG(INFO) << "[BatchGet] key=" << desc.key
                  << " store_id=" << desc.store_id
                  << " offset=" << desc.offset
                  << " size=" << desc.size
                  << " access_type=" << static_cast<int>(desc.access_type);

        int channel = ChannelForAccessType(desc.access_type);
        uint32_t store_id = desc.store_id;
        std::string remote_addr = RemoteAddrForDesc(desc);

        // Scheduler: RequestIO
        uint64_t request_ts = GetCurrentTimeNs();
        IOResponseData io_resp;
        if (scheduler_enabled_) {
            IORequestData io_req;
            io_req.io_channel = channel;
            io_req.store_id = store_id;
            io_req.io_size = buf.size;
            io_req.request_ts_ns = request_ts;
            io_req.remote_node_addr = remote_addr;
            io_resp = scheduler_proxy_->RequestIO(io_req);
        }

        // Execute read
        uint64_t start_ts = GetCurrentTimeNs();
        Status read_status = DoRead(desc, buf.data_ptr, buf.size);
        uint64_t done_ts = GetCurrentTimeNs();

        if (!read_status.ok()) {
            LOG(WARNING) << "[BatchGet] DoRead FAILED for key=" << desc.key
                         << " status=" << read_status.ToString();
        }

        // Scheduler: ReportIOCompletion
        if (scheduler_enabled_) {
            IOCompletionData report;
            report.ticket = io_resp.ticket;
            report.io_start_ts_ns = start_ts;
            report.io_done_ts_ns = done_ts;
            report.io_size = buf.size;
            report.io_channel = channel;
            report.io_status = read_status.ok() ? 0 : -1;
            report.store_id = store_id;
            report.remote_node_addr = remote_addr;
            scheduler_proxy_->ReportIOCompletion(report);
        }

        if (read_status.ok()) {
            bytes_read[i] = static_cast<int32_t>(buf.size);
        } else {
            bytes_read[i] = -1;
        }
    }

    return bytes_read;
}

std::vector<int32_t> FalconKVClientImpl::BatchGetSync(
    const std::vector<std::string>& keys,
    const std::vector<BufferInfo>& buffers) {

    if (keys.size() != buffers.size()) {
        LOG(ERROR) << "[FalconKVClient] BatchGetSync: keys.size()=" << keys.size()
                   << " != buffers.size()=" << buffers.size();
        return std::vector<int32_t>(keys.size(), -1);
    }

    // Step 1: Lookup key descriptors from cache
    std::vector<KeyDescriptor> hit_descs;
    std::vector<std::string> missing_keys;
    key_desc_cache_.BatchLookup(keys, hit_descs, missing_keys);

    LOG(INFO) << "[BatchGetSync] keys=" << keys.size()
              << " cache_hits=" << hit_descs.size()
              << " missing=" << missing_keys.size();

    // Build a map of key -> descriptor for cache hits
    std::unordered_map<std::string, KeyDescriptor> hit_map;
    for (const auto& desc : hit_descs) {
        hit_map.emplace(desc.key, desc);
    }

    // Step 2: For missing keys, query remote MetaManager
    if (!missing_keys.empty()) {
        auto records = meta_client_.BatchLookup(missing_keys);
        for (size_t i = 0; i < records.size(); ++i) {
            if (!records[i].key.empty()) {
                KeyDescriptor desc(records[i].key);
                desc.store_id = records[i].store_id;
                desc.offset = records[i].offset;
                desc.size = records[i].size;
                desc.access_time_ms = records[i].access_time_ms;
                // Determine access type: same node -> NODE_DIRECT, else REMOTE_RPC
                if (records[i].node_id != 0 && records[i].node_id == node_id_) {
                    desc.access_type = AccessType::ACCESS_NODE_DIRECT;
                    // Register the store's data file with NodeLocalAccessor
                    if (!records[i].data_file.empty()) {
                        node_accessor_.RegisterStoreFile(
                            records[i].store_id, records[i].data_file);
                    }
                } else {
                    desc.access_type = AccessType::ACCESS_REMOTE_RPC;
                }
                // Populate store_addr_map_ and desc.store_addr for RPC routing
                if (!records[i].store_addr.empty()) {
                    store_addr_map_[records[i].store_id] = records[i].store_addr;
                    desc.store_addr = records[i].store_addr;
                }
                hit_map.emplace(records[i].key, desc);
                key_desc_cache_.Insert(records[i].key, desc);
            }
        }
    }

    // Step 3: Build ordered key_descs matching the input keys order
    std::vector<KeyDescriptor> ordered_descs;
    std::vector<BufferInfo> ordered_bufs;
    std::vector<size_t> valid_indices;

    for (size_t i = 0; i < keys.size(); ++i) {
        auto it = hit_map.find(keys[i]);
        if (it != hit_map.end()) {
            ordered_descs.push_back(it->second);
            ordered_bufs.push_back(buffers[i]);
            valid_indices.push_back(i);
        }
    }

    // Step 4: Batch read
    auto read_results = BatchGet(ordered_descs, ordered_bufs);

    // Step 5: Map results back to original order
    std::vector<int32_t> results(keys.size(), -1);
    for (size_t i = 0; i < valid_indices.size(); ++i) {
        results[valid_indices[i]] = read_results[i];
    }

    return results;
}

void FalconKVClientImpl::Close() {
    meta_client_.StopReconnectLoop();
    node_accessor_.Close();
    key_desc_cache_.Clear();
    store_rpc_mgr_.CloseAll();
}

} // namespace falconkv

#include "src/store/meta_sync_client.h"

#include <brpc/controller.h>

#include "src/common/logging.h"

namespace falconkv {

MetaSyncClient::MetaSyncClient() = default;

MetaSyncClient::~MetaSyncClient() {
    StopReconnectLoop();
}

// -----------------------------------------------------------------
// Connect / TryConnect
// -----------------------------------------------------------------

Status MetaSyncClient::TryConnect() {
    if (meta_addr_.empty()) {
        return Status::OK();
    }

    auto new_channel = std::make_unique<brpc::Channel>();
    brpc::ChannelOptions options;
    options.timeout_ms = 5000;
    options.connect_timeout_ms = 3000;
    options.max_retry = 3;

    int rc = new_channel->Init(meta_addr_.c_str(), &options);
    if (rc != 0) {
        return Status::RpcError("MetaSyncClient: failed to connect to meta at " +
                                meta_addr_);
    }

    // Swap in the new channel and create a new stub.
    channel_ = std::move(new_channel);
    stub_ = std::make_unique<FalconKVMetaService_Stub>(channel_.get());
    connected_.store(true);
    return Status::OK();
}

Status MetaSyncClient::Connect(const std::string& meta_addr) {
    meta_addr_ = meta_addr;
    if (meta_addr_.empty()) {
        return Status::OK();
    }

    Status s = TryConnect();
    if (!s.ok()) {
        // Log but don't fail — Store can still start in local-only mode.
        LOG(WARNING) << "[MetaSyncClient] Connect failed: " << s.ToString()
                     << " (will retry later)";
    } else {
        FullResync();
    }
    return Status::OK();
}

// -----------------------------------------------------------------
// SetStoreInfo / SetMetaIndex
// -----------------------------------------------------------------

void MetaSyncClient::SetStoreInfo(uint32_t store_id, uint32_t node_id,
                                   const std::string& data_file,
                                   uint64_t capacity_bytes, uint32_t chunk_size) {
    store_id_ = store_id;
    node_id_ = node_id;
    data_file_ = data_file;
    capacity_bytes_ = capacity_bytes;
    chunk_size_ = chunk_size;
}

void MetaSyncClient::SetMetaIndex(StoreMetaIndex* meta_index) {
    meta_index_ = meta_index;
}

void MetaSyncClient::SetStoreRpcAddr(const std::string& host, uint32_t port) {
    store_rpc_host_ = host;
    store_rpc_port_ = port;
}

// -----------------------------------------------------------------
// SyncCommit
// -----------------------------------------------------------------

Status MetaSyncClient::SyncCommit(uint32_t store_id,
                                   const std::vector<StoreKeyRecord>& records) {
    if (!connected_.load() || !stub_) {
        return Status::OK(); // skip if not connected
    }

    SyncCommitRequest request;
    request.set_store_id(store_id);
    for (const auto& rec : records) {
        auto* desc = request.add_key_records();
        desc->set_key(rec.key);
        desc->set_store_id(store_id);
        desc->set_offset(rec.offset);
        desc->set_size(rec.size);
        desc->set_access_type(ACCESS_LOCAL_DIRECT);
    }

    SyncCommitResponse response;
    brpc::Controller cntl;

    stub_->SyncCommit(&cntl, &request, &response, nullptr);

    if (cntl.Failed()) {
        connected_.store(false);
        // Retry once — brpc lazy connect may not have established TCP yet.
        brpc::Controller cntl2;
        stub_->SyncCommit(&cntl2, &request, &response, nullptr);
        if (cntl2.Failed()) {
            LOG(ERROR) << "[MetaSyncClient] SyncCommit RPC failed (retry also failed): "
                       << cntl2.ErrorText();
            return Status::RpcError("SyncCommit RPC failed: " +
                                    std::string(cntl2.ErrorText()));
        }
        connected_.store(true);
    }

    if (response.status() != 0) {
        LOG(ERROR) << "[MetaSyncClient] SyncCommit failed with status="
                   << response.status();
        return Status::Corruption("SyncCommit failed with status: " +
                                  std::to_string(response.status()));
    }

    return Status::OK();
}

// -----------------------------------------------------------------
// SyncRemove
// -----------------------------------------------------------------

Status MetaSyncClient::SyncRemove(uint32_t store_id,
                                   const std::vector<std::string>& keys) {
    if (!connected_.load() || !stub_) {
        return Status::OK(); // skip if not connected
    }

    SyncRemoveRequest request;
    request.set_store_id(store_id);
    for (const auto& key : keys) {
        request.add_keys(key);
    }

    SyncRemoveResponse response;
    brpc::Controller cntl;

    stub_->SyncRemove(&cntl, &request, &response, nullptr);

    if (cntl.Failed()) {
        connected_.store(false);
        LOG(ERROR) << "[MetaSyncClient] SyncRemove RPC failed: " << cntl.ErrorText();
        return Status::RpcError("SyncRemove RPC failed: " +
                                std::string(cntl.ErrorText()));
    }

    if (response.status() != 0) {
        LOG(ERROR) << "[MetaSyncClient] SyncRemove failed with status="
                   << response.status();
        return Status::Corruption("SyncRemove failed with status: " +
                                  std::to_string(response.status()));
    }

    return Status::OK();
}

// -----------------------------------------------------------------
// RegisterStore
// -----------------------------------------------------------------

Status MetaSyncClient::RegisterStore(uint32_t store_id, uint32_t node_id,
                                      const std::string& data_file,
                                      uint64_t capacity_bytes,
                                      uint32_t chunk_size) {
    if (!connected_.load() || !stub_) {
        return Status::OK(); // skip if not connected
    }

    StoreRegisterRequest request;
    request.set_store_id(store_id);
    request.set_node_id(node_id);
    request.set_data_file(data_file);
    request.set_capacity_bytes(capacity_bytes);
    request.set_chunk_size(chunk_size);
    if (store_rpc_port_ > 0) {
        request.set_node_host(store_rpc_host_);
        request.set_node_port(store_rpc_port_);
    }

    StoreRegisterResponse response;
    brpc::Controller cntl;

    stub_->StoreRegister(&cntl, &request, &response, nullptr);

    if (cntl.Failed()) {
        connected_.store(false);
        LOG(ERROR) << "[MetaSyncClient] RegisterStore RPC failed: " << cntl.ErrorText();
        return Status::RpcError("RegisterStore RPC failed: " +
                                std::string(cntl.ErrorText()));
    }

    if (response.status() != 0) {
        LOG(ERROR) << "[MetaSyncClient] RegisterStore failed: " << response.error_msg();
        return Status::Corruption("RegisterStore failed: " +
                                  response.error_msg());
    }

    return Status::OK();
}

// -----------------------------------------------------------------
// Reconnect loop
// -----------------------------------------------------------------

void MetaSyncClient::StartReconnectLoop(int interval_sec) {
    if (reconnect_running_.load()) {
        return;
    }
    reconnect_running_.store(true);
    reconnect_thread_ = std::thread(&MetaSyncClient::ReconnectLoop, this, interval_sec);
}

void MetaSyncClient::StopReconnectLoop() {
    if (!reconnect_running_.load()) {
        return;
    }
    reconnect_running_.store(false);
    reconnect_cv_.notify_all();
    if (reconnect_thread_.joinable()) {
        reconnect_thread_.join();
    }
}

void MetaSyncClient::ReconnectLoop(int interval_sec) {
    while (reconnect_running_.load()) {
        // Wait with timeout so we can check running_ periodically.
        {
            std::unique_lock<std::mutex> lock(reconnect_mutex_);
            reconnect_cv_.wait_for(lock,
                                    std::chrono::seconds(interval_sec),
                                    [this] { return !reconnect_running_.load(); });
        }

        if (!reconnect_running_.load()) {
            break;
        }

        // Already connected — nothing to do.
        if (connected_.load()) {
            continue;
        }

        // Try to reconnect.
        Status s = TryConnect();
        if (s.ok()) {
            LOG(INFO) << "[MetaSyncClient] Reconnected to Meta at " << meta_addr_;
            FullResync();
        }
    }
}

// -----------------------------------------------------------------
// FullResync
// -----------------------------------------------------------------

void MetaSyncClient::FullResync() {
    // 1. Register the store (with retry — brpc channel may not have
    //    established the TCP connection yet on the first attempt).
    Status s;
    for (int attempt = 0; attempt < 3; ++attempt) {
        s = RegisterStore(store_id_, node_id_, data_file_,
                          capacity_bytes_, chunk_size_);
        if (s.ok()) break;
        LOG(WARNING) << "[MetaSyncClient] FullResync: RegisterStore attempt "
                     << (attempt + 1) << " failed: " << s.ToString();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    if (!s.ok()) {
        LOG(ERROR) << "[MetaSyncClient] FullResync: RegisterStore failed after "
                   << "3 attempts: " << s.ToString();
        connected_.store(false);
        return;
    }

    // 2. Get all committed entries and push them in batches.
    if (!meta_index_) {
        return;
    }

    std::vector<StoreKeyRecord> entries = meta_index_->GetAllCommittedEntries();
    if (entries.empty()) {
        return;
    }

    constexpr size_t kBatchSize = 256;
    for (size_t i = 0; i < entries.size(); i += kBatchSize) {
        size_t end = std::min(i + kBatchSize, entries.size());
        std::vector<StoreKeyRecord> batch(entries.begin() + i, entries.begin() + end);

        s = SyncCommit(store_id_, batch);
        if (!s.ok()) {
            LOG(ERROR) << "[MetaSyncClient] FullResync: SyncCommit batch failed: "
                       << s.ToString();
            // connected_ is already set to false by SyncCommit on failure.
            return;
        }
    }

    LOG(INFO) << "[MetaSyncClient] FullResync completed: " << entries.size()
              << " keys pushed";
}

} // namespace falconkv

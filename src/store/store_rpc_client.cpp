#include "src/store/store_rpc_client.h"

#include <brpc/controller.h>

#include "src/common/logging.h"

namespace falconkv {

StoreRpcClient::StoreRpcClient() = default;

StoreRpcClient::~StoreRpcClient() = default;

Status StoreRpcClient::Connect(const std::string& addr) {
    if (addr.empty()) {
        LOG(ERROR) << "[StoreRpcClient] Connect: empty store address";
        return Status::InvalidArg("empty store address");
    }

    brpc::ChannelOptions options;
    options.connect_timeout_ms = 3000;
    options.timeout_ms = 30000;       // 30s for large batch reads
    options.max_retry = 1;

    int rc = channel_.Init(addr.c_str(), &options);
    if (rc != 0) {
        LOG(ERROR) << "[StoreRpcClient] Connect: failed to init brpc channel to store at "
                   << addr << ", rc=" << rc;
        return Status::RpcError("failed to init brpc channel to store at " +
                                addr);
    }

    stub_ = std::make_unique<FalconKVStoreService_Stub>(&channel_);
    connected_ = true;
    LOG(INFO) << "[StoreRpcClient] Connected to store at " << addr;
    return Status::OK();
}

Status StoreRpcClient::Read(uint64_t offset, void* buffer, uint32_t size,
                             uint32_t client_id,
                             const std::string& source_node_addr) {
    if (!connected_) {
        LOG(ERROR) << "[StoreRpcClient] Read: not connected";
        return Status::RpcError("not connected");
    }

    ReadRequest request;
    request.set_offset(offset);
    request.set_size(size);
    request.set_client_id(client_id);
    request.set_source_node_addr(source_node_addr);

    ReadResponse response;
    brpc::Controller cntl;

    stub_->Read(&cntl, &request, &response, nullptr);

    if (cntl.Failed()) {
        LOG(ERROR) << "[StoreRpcClient] Read RPC failed at offset " << offset
                   << ", size=" << size << ": " << cntl.ErrorText();
        return Status::RpcError("Store Read RPC failed: " +
                                std::string(cntl.ErrorText()));
    }

    if (response.status() != 0) {
        LOG(ERROR) << "[StoreRpcClient] Read failed at offset " << offset
                   << " with status=" << response.status();
        return Status::IoError("Store Read failed with status: " +
                               std::to_string(response.status()));
    }

    uint32_t bytes_read = response.bytes_read();
    if (bytes_read > size) {
        bytes_read = size;
    }

    // Read data from brpc attachment (zero-copy path from server)
    if (bytes_read > 0) {
        cntl.response_attachment().copy_to(buffer, bytes_read);
    }
    return Status::OK();
}

Status StoreRpcClient::BatchRead(const std::vector<uint64_t>& offsets,
                                 const std::vector<uint32_t>& sizes,
                                 const std::vector<void*>& buffers,
                                 std::vector<int32_t>& results) {
    if (!connected_) {
        LOG(ERROR) << "[StoreRpcClient] BatchRead: not connected";
        return Status::RpcError("not connected");
    }

    size_t n = offsets.size();
    if (n != sizes.size() || n != buffers.size()) {
        LOG(ERROR) << "[StoreRpcClient] BatchRead: size mismatch, offsets="
                   << n << " sizes=" << sizes.size()
                   << " buffers=" << buffers.size();
        return Status::InvalidArg("BatchRead parameter size mismatch");
    }

    results.assign(n, 0);

    if (n == 0) {
        return Status::OK();
    }

    BatchReadRequest request;
    for (size_t i = 0; i < n; ++i) {
        auto* seg = request.add_segments();
        seg->set_offset(offsets[i]);
        seg->set_size(sizes[i]);
    }

    BatchReadResponse response;
    brpc::Controller cntl;

    stub_->BatchRead(&cntl, &request, &response, nullptr);

    if (cntl.Failed()) {
        LOG(ERROR) << "[StoreRpcClient] BatchRead RPC failed (" << n
                   << " segments): " << cntl.ErrorText();
        results.assign(n, -1);
        return Status::RpcError("Store BatchRead RPC failed: " +
                                std::string(cntl.ErrorText()));
    }

    if (response.status() != 0) {
        LOG(ERROR) << "[StoreRpcClient] BatchRead failed with status="
                   << response.status();
        results.assign(n, -1);
        return Status::IoError("Store BatchRead failed with status: " +
                               std::to_string(response.status()));
    }

    int seg_count = response.bytes_read_size();
    if (static_cast<size_t>(seg_count) != n) {
        LOG(ERROR) << "[StoreRpcClient] BatchRead: expected " << n
                   << " segments, got " << seg_count;
        results.assign(n, -1);
        return Status::IoError("BatchRead segment count mismatch");
    }

    // Read data from brpc attachment (zero-copy path from server).
    // Server lays out segments sequentially: [seg0][seg1]...[segN].
    // bytes_read[i] tells us each segment's size in the attachment.
    size_t att_offset = 0;
    for (int i = 0; i < seg_count; ++i) {
        uint32_t bytes_read = response.bytes_read(i);
        if (bytes_read == 0) {
            results[i] = -1;
            continue;
        }
        uint32_t to_copy = std::min(bytes_read, sizes[i]);
        cntl.response_attachment().copy_to(buffers[i], to_copy, att_offset);
        att_offset += bytes_read;
        results[i] = static_cast<int32_t>(to_copy);
    }

    return Status::OK();
}

Status StoreRpcClient::Ping() {
    if (!connected_) {
        LOG(ERROR) << "[StoreRpcClient] Ping: not connected";
        return Status::RpcError("not connected");
    }

    PingRequest request;
    PongResponse response;
    brpc::Controller cntl;

    stub_->Ping(&cntl, &request, &response, nullptr);

    if (cntl.Failed()) {
        LOG(ERROR) << "[StoreRpcClient] Ping RPC failed: " << cntl.ErrorText();
        return Status::RpcError("Store Ping RPC failed: " +
                                std::string(cntl.ErrorText()));
    }

    return Status::OK();
}

} // namespace falconkv

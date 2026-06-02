#include "src/store/store_rpc_client.h"

#include <brpc/controller.h>

#include <cstring>

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
    options.timeout_ms = 5000;
    options.max_retry = 3;

    int rc = channel_.Init(addr.c_str(), &options);
    if (rc != 0) {
        LOG(ERROR) << "[StoreRpcClient] Connect: failed to init brpc channel to store at "
                   << addr << ", rc=" << rc;
        return Status::RpcError("failed to init brpc channel to store at " +
                                addr);
    }

    stub_ = std::make_unique<FalconKVStoreService_Stub>(&channel_);
    connected_ = true;
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

    std::memcpy(buffer, response.data().data(), bytes_read);
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

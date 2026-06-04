#include "src/store/store_server.h"

#include "src/common/logging.h"

namespace falconkv {

StoreServer::StoreServer(const FalconKVStore::Config& store_config,
                          const std::string& listen_addr)
    : listen_addr_(listen_addr),
      store_(store_config),
      service_impl_(&store_) {}

StoreServer::~StoreServer() {
    Stop();
}

Status StoreServer::Start() {
    // Initialize the store's data file
    Status s = store_.Init();
    if (!s.ok()) {
        return s;
    }

    // Add the service to the server
    int rc = server_.AddService(&service_impl_,
                                 brpc::SERVER_DOESNT_OWN_SERVICE);
    if (rc != 0) {
        return Status::RpcError("failed to add StoreService to brpc server");
    }

    // Start listening
    brpc::ServerOptions options;
    rc = server_.Start(listen_addr_.c_str(), &options);
    if (rc != 0) {
        return Status::RpcError("failed to start Store server on " +
                                listen_addr_);
    }

    LOG(INFO) << "[StoreServer] Started on " << listen_addr_;
    return Status::OK();
}

void StoreServer::Stop() {
    if (!server_.IsRunning()) {
        return;
    }
    server_.Stop(0 /* timeout_ms, 0 = immediate */);
    server_.Join();
    store_.Close();
}

} // namespace falconkv

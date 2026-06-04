#include "src/meta/meta_server.h"

#include "src/common/logging.h"

namespace falconkv {

MetaServer::MetaServer(const MetaConfig& config,
                       const std::string& meta_addr)
    : config_(config),
      listen_addr_(meta_addr),
      service_impl_(&meta_manager_) {}

MetaServer::~MetaServer() {
    Stop();
}

Status MetaServer::Start() {
    // Add the service to the server.  The server does NOT own the service.
    int rc = server_.AddService(&service_impl_,
                                brpc::SERVER_DOESNT_OWN_SERVICE);
    if (rc != 0) {
        return Status::RpcError("failed to add MetaService to brpc server");
    }

    // Start listening.
    brpc::ServerOptions options;
    rc = server_.Start(listen_addr_.c_str(), &options);
    if (rc != 0) {
        return Status::RpcError("failed to start Meta server on " +
                                listen_addr_);
    }

    LOG(INFO) << "[MetaServer] Started on " << listen_addr_;
    return Status::OK();
}

void MetaServer::Stop() {
    if (!server_.IsRunning()) {
        return;
    }
    server_.Stop(0 /* timeout_ms, 0 = immediate */);
    server_.Join();
}

} // namespace falconkv

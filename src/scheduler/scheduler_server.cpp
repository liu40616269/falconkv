#include "src/scheduler/scheduler_server.h"

#include "src/common/logging.h"

namespace falconkv {

SchedulerServer::SchedulerServer(const SchedulerConfig& config)
    : config_(config),
      uds_addr_("unix:" + config.uds_path),
      scheduler_(config),
      service_impl_(&scheduler_) {}

SchedulerServer::~SchedulerServer() {
    Stop();
}

Status SchedulerServer::Start() {
    // Start the scheduler internals (stats thread, etc.).
    Status s = scheduler_.Start();
    if (!s.ok()) {
        return s;
    }

    // Add the service to the brpc server.
    int rc = server_.AddService(&service_impl_,
                                brpc::SERVER_DOESNT_OWN_SERVICE);
    if (rc != 0) {
        scheduler_.Stop();
        return Status::RpcError("failed to add SchedulerService to brpc server");
    }

    // Start listening on the Unix domain socket.
    brpc::ServerOptions options;
    rc = server_.Start(uds_addr_.c_str(), &options);
    if (rc != 0) {
        scheduler_.Stop();
        return Status::RpcError("failed to start Scheduler server on " +
                                uds_addr_);
    }

    LOG(INFO) << "SchedulerServer: listening on " << uds_addr_;
    return Status::OK();
}

void SchedulerServer::Stop() {
    if (!server_.IsRunning()) {
        return;
    }

    server_.Stop(0 /* timeout_ms, 0 = immediate */);
    server_.Join();
    scheduler_.Stop();
}

} // namespace falconkv

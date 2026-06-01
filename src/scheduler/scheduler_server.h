#pragma once

#include <string>
#include <memory>

#include <brpc/server.h>

#include "src/common/config.h"
#include "src/scheduler/scheduler_service.h"
#include "src/scheduler/scheduler_service_impl.h"

namespace falconkv {

// Encapsulates a standalone Scheduler server process.
// Owns FalconKVScheduler + SchedulerServiceImpl + brpc::Server.
class SchedulerServer {
public:
    explicit SchedulerServer(const SchedulerConfig& config);
    ~SchedulerServer();

    // Non-copyable
    SchedulerServer(const SchedulerServer&) = delete;
    SchedulerServer& operator=(const SchedulerServer&) = delete;

    /// Start the scheduler and the RPC server.
    Status Start();

    /// Stop the RPC server and the scheduler.
    void Stop();

    /// Get the underlying FalconKVScheduler.
    FalconKVScheduler* GetScheduler() { return &scheduler_; }

    /// Get the UDS path the server is listening on.
    const std::string& UDSPath() const { return uds_addr_; }

private:
    SchedulerConfig config_;
    std::string uds_addr_;
    FalconKVScheduler scheduler_;
    SchedulerServiceImpl service_impl_;
    brpc::Server server_;
};

} // namespace falconkv

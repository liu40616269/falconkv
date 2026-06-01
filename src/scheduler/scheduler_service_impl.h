#pragma once

#include "falconkv_scheduler.pb.h"
#include "src/scheduler/scheduler_service.h"

namespace falconkv {

// Server-side implementation of FalconKVSchedulerService.
// Each RPC method extracts proto request fields, delegates to FalconKVScheduler,
// and converts C++ results back to proto response.
class SchedulerServiceImpl : public FalconKVSchedulerService {
public:
    explicit SchedulerServiceImpl(FalconKVScheduler* scheduler);
    ~SchedulerServiceImpl() override = default;

    void RequestIO(::google::protobuf::RpcController* controller,
                   const IORequest* request,
                   IOResponse* response,
                   ::google::protobuf::Closure* done) override;

    void ReportIOCompletion(::google::protobuf::RpcController* controller,
                            const IOCompletionReport* request,
                            IOCompletionAck* response,
                            ::google::protobuf::Closure* done) override;

    void StoreReportIO(::google::protobuf::RpcController* controller,
                       const StoreIOReport* request,
                       StoreIOAck* response,
                       ::google::protobuf::Closure* done) override;

    void Heartbeat(::google::protobuf::RpcController* controller,
                   const HeartbeatRequest* request,
                   HeartbeatResponse* response,
                   ::google::protobuf::Closure* done) override;

private:
    FalconKVScheduler* scheduler_; // not owned
};

} // namespace falconkv

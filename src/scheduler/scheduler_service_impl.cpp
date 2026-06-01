#include "src/scheduler/scheduler_service_impl.h"

#include <brpc/closure_guard.h>
#include <brpc/controller.h>
#include <chrono>

namespace falconkv {

SchedulerServiceImpl::SchedulerServiceImpl(FalconKVScheduler* scheduler)
    : scheduler_(scheduler) {}

// -----------------------------------------------------------------
// RequestIO
// -----------------------------------------------------------------
void SchedulerServiceImpl::RequestIO(::google::protobuf::RpcController*,
                                     const IORequest* request,
                                     IOResponse* response,
                                     ::google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);

    IORequestData data;
    data.client_id = request->client_id();
    data.io_channel = static_cast<int>(request->io_channel());
    data.store_id = request->store_id();
    data.io_size = request->io_size();
    data.priority = request->priority();
    data.request_ts_ns = request->request_ts_ns();
    data.remote_node_addr = request->remote_node_addr();

    IOResponseData result = scheduler_->HandleRequestIO(data);

    response->set_status(result.status);
    response->set_permitted_ts_ns(result.permitted_ts_ns);
    response->set_ticket(result.ticket);
}

// -----------------------------------------------------------------
// ReportIOCompletion
// -----------------------------------------------------------------
void SchedulerServiceImpl::ReportIOCompletion(
    ::google::protobuf::RpcController*,
    const IOCompletionReport* request,
    IOCompletionAck* response,
    ::google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);

    IOCompletionData data;
    data.client_id = request->client_id();
    data.ticket = request->ticket();
    data.io_start_ts_ns = request->io_start_ts_ns();
    data.io_done_ts_ns = request->io_done_ts_ns();
    data.io_size = request->io_size();
    data.io_channel = static_cast<int>(request->io_channel());
    data.io_status = request->io_status();
    data.store_id = request->store_id();
    data.remote_node_addr = request->remote_node_addr();

    scheduler_->HandleIOCompletion(data);
    response->set_status(0);
}

// -----------------------------------------------------------------
// StoreReportIO
// -----------------------------------------------------------------
void SchedulerServiceImpl::StoreReportIO(::google::protobuf::RpcController*,
                                         const StoreIOReport* request,
                                         StoreIOAck* response,
                                         ::google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);

    IOCompletionData data;
    data.client_id = request->source_client_id();
    data.store_id = request->store_id();
    data.io_channel = static_cast<int>(request->io_channel());
    data.io_size = request->io_size();
    data.io_start_ts_ns = request->request_ts_ns();
    data.io_done_ts_ns = request->done_ts_ns();
    data.remote_node_addr = request->source_node_addr();

    scheduler_->HandleIOCompletion(data);
    response->set_status(0);
}

// -----------------------------------------------------------------
// Heartbeat
// -----------------------------------------------------------------
void SchedulerServiceImpl::Heartbeat(::google::protobuf::RpcController*,
                                     const HeartbeatRequest* /*request*/,
                                     HeartbeatResponse* response,
                                     ::google::protobuf::Closure* done) {
    brpc::ClosureGuard done_guard(done);

    auto tp = std::chrono::steady_clock::now().time_since_epoch();
    uint64_t now_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(tp).count());

    response->set_status(0);
    response->set_timestamp_ns(now_ns);
}

} // namespace falconkv

#include "src/scheduler/scheduler_proxy.h"

#include <chrono>
#include <atomic>

#include "src/common/logging.h"

#ifdef FALCONKV_HAS_BRPC
#include <brpc/controller.h>
#endif

namespace falconkv {

namespace {

uint64_t NowNanos() {
    auto tp = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(tp).count());
}

// Simple atomic counter for generating bypass tickets (never conflicts with
// real scheduler tickets because it uses the high bit).
std::atomic<uint64_t> g_bypass_ticket_counter{0};

uint64_t NextBypassTicket() {
    return g_bypass_ticket_counter.fetch_add(1, std::memory_order_relaxed) |
           (1ULL << 63);
}

}  // namespace

// ---------------------------------------------------------------------------
// SchedulerProxy
// ---------------------------------------------------------------------------

SchedulerProxy::SchedulerProxy(const std::string& uds_path,
                               int rpc_timeout_ms,
                               int max_consecutive_failures,
                               int reconnect_interval_sec)
    : uds_path_(uds_path),
      state_(State::DISCONNECTED),
      max_consecutive_failures_(max_consecutive_failures),
      reconnect_interval_sec_(reconnect_interval_sec),
      rpc_timeout_ms_(rpc_timeout_ms) {
    // Attempt an initial probe.
    if (ProbeScheduler()) {
        state_.store(State::CONNECTED, std::memory_order_release);
    } else {
        // Scheduler unreachable at startup — enter bypass mode immediately.
        state_.store(State::BYPASS, std::memory_order_release);
        StartReconnectProbe();
    }
}

SchedulerProxy::~SchedulerProxy() {
    stopped_.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(reconnect_mutex_);
        if (reconnect_thread_.joinable()) {
            reconnect_thread_.join();
        }
    }
}

#ifdef FALCONKV_HAS_BRPC

bool SchedulerProxy::Connect() {
    if (!channel_) {
        channel_ = std::make_unique<brpc::Channel>();
    }
    brpc::ChannelOptions options;
    options.timeout_ms = rpc_timeout_ms_;
    options.protocol = brpc::PROTOCOL_BAIDU_STD;
    std::string addr = "unix:" + uds_path_;
    if (channel_->Init(addr.c_str(), &options) != 0) {
        channel_.reset();
        stub_.reset();
        return false;
    }
    stub_ = std::make_unique<FalconKVSchedulerService_Stub>(channel_.get());
    return true;
}

bool SchedulerProxy::ProbeScheduler() {
    if (!Connect()) return false;

    HeartbeatRequest req;
    req.set_timestamp_ns(NowNanos());
    HeartbeatResponse resp;
    brpc::Controller cntl;
    stub_->Heartbeat(&cntl, &req, &resp, nullptr);
    if (cntl.Failed()) {
        return false;
    }
    return true;
}

IOResponseData SchedulerProxy::RequestIO(const IORequestData& request) {
    State s = state_.load(std::memory_order_acquire);

    if (s == State::BYPASS) {
        return MakeBypassResponse(request);
    }

    if (s == State::DISCONNECTED) {
        // Try once to connect.
        if (ProbeScheduler()) {
            state_.store(State::CONNECTED, std::memory_order_release);
        } else {
            StartReconnectProbe();
            return MakeBypassResponse(request);
        }
    }

    // state == CONNECTED — send real RPC.
    if (!stub_) {
        // Stub unexpectedly null, fall back to bypass.
        StartReconnectProbe();
        return MakeBypassResponse(request);
    }

    IORequest io_req;
    io_req.set_client_id(request.client_id);
    io_req.set_io_channel(static_cast<IOChannel>(request.io_channel));
    io_req.set_store_id(request.store_id);
    io_req.set_io_size(request.io_size);
    io_req.set_priority(request.priority);
    io_req.set_request_ts_ns(request.request_ts_ns);
    io_req.set_remote_node_addr(request.remote_node_addr);

    IOResponse io_resp;
    brpc::Controller cntl;
    cntl.set_timeout_ms(rpc_timeout_ms_);
    stub_->RequestIO(&cntl, &io_req, &io_resp, nullptr);

    if (cntl.Failed()) {
        int failures = consecutive_failures_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (failures >= max_consecutive_failures_) {
            state_.store(State::BYPASS, std::memory_order_release);
            StartReconnectProbe();
        }
        return MakeBypassResponse(request);
    }

    // RPC succeeded.
    consecutive_failures_.store(0, std::memory_order_relaxed);
    IOResponseData resp;
    resp.status = io_resp.status();
    resp.permitted_ts_ns = io_resp.permitted_ts_ns();
    resp.ticket = io_resp.ticket();
    return resp;
}

void SchedulerProxy::ReportIOCompletion(const IOCompletionData& report) {
    State s = state_.load(std::memory_order_acquire);
    if (s == State::BYPASS) {
        return;
    }

    if (!stub_) {
        return;
    }

    IOCompletionReport proto_report;
    proto_report.set_client_id(report.client_id);
    proto_report.set_ticket(report.ticket);
    proto_report.set_io_start_ts_ns(report.io_start_ts_ns);
    proto_report.set_io_done_ts_ns(report.io_done_ts_ns);
    proto_report.set_io_size(report.io_size);
    proto_report.set_io_channel(static_cast<IOChannel>(report.io_channel));
    proto_report.set_io_status(report.io_status);
    proto_report.set_store_id(report.store_id);
    proto_report.set_remote_node_addr(report.remote_node_addr);

    IOCompletionAck ack;
    brpc::Controller cntl;
    cntl.set_timeout_ms(rpc_timeout_ms_);
    stub_->ReportIOCompletion(&cntl, &proto_report, &ack, nullptr);

    if (cntl.Failed()) {
        int failures = consecutive_failures_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (failures >= max_consecutive_failures_) {
            state_.store(State::BYPASS, std::memory_order_release);
            StartReconnectProbe();
        }
    } else {
        consecutive_failures_.store(0, std::memory_order_relaxed);
    }
}

void SchedulerProxy::StoreReportIOAsync(uint32_t store_id,
                                         int io_channel,
                                         uint32_t source_client_id,
                                         uint64_t io_size,
                                         uint64_t request_ts_ns,
                                         uint64_t done_ts_ns,
                                         const std::string& source_node_addr) {
    State s = state_.load(std::memory_order_acquire);
    if (s == State::BYPASS) {
        return;
    }

    if (!stub_) {
        return;
    }

    StoreIOReport report;
    report.set_store_id(store_id);
    report.set_io_channel(static_cast<IOChannel>(io_channel));
    report.set_source_client_id(source_client_id);
    report.set_io_size(io_size);
    report.set_request_ts_ns(request_ts_ns);
    report.set_done_ts_ns(done_ts_ns);
    report.set_source_node_addr(source_node_addr);

    // Fire-and-forget: spawn a detached thread for the RPC call.
    auto stub_ptr = stub_.get();
    int timeout = rpc_timeout_ms_;
    std::thread([stub_ptr, report = std::move(report), timeout]() {
        StoreIOAck ack;
        brpc::Controller cntl;
        cntl.set_timeout_ms(timeout);
        stub_ptr->StoreReportIO(&cntl, &report, &ack, nullptr);
        if (cntl.Failed()) {
            LOG(WARNING) << "SchedulerProxy: StoreReportIOAsync RPC failed: "
                         << cntl.ErrorText();
        }
    }).detach();
}

#else  // !FALCONKV_HAS_BRPC

// ---------------------------------------------------------------------------
// Fallback: mock implementation when brpc is not available.
// ---------------------------------------------------------------------------

bool SchedulerProxy::ProbeScheduler() {
    return false;
}

IOResponseData SchedulerProxy::RequestIO(const IORequestData& request) {
    State s = state_.load(std::memory_order_acquire);

    if (s == State::BYPASS) {
        return MakeBypassResponse(request);
    }

    // In mock mode without brpc, always return bypass.
    StartReconnectProbe();
    return MakeBypassResponse(request);
}

void SchedulerProxy::ReportIOCompletion(const IOCompletionData& report) {
    (void)report;
}

void SchedulerProxy::StoreReportIOAsync(uint32_t /*store_id*/,
                                         int /*io_channel*/,
                                         uint32_t /*source_client_id*/,
                                         uint64_t /*io_size*/,
                                         uint64_t /*request_ts_ns*/,
                                         uint64_t /*done_ts_ns*/,
                                         const std::string& /*source_node_addr*/) {
}

#endif  // FALCONKV_HAS_BRPC

bool SchedulerProxy::IsBypassMode() const {
    return state_.load(std::memory_order_acquire) == State::BYPASS;
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

IOResponseData SchedulerProxy::MakeBypassResponse(const IORequestData& /*request*/) {
    IOResponseData resp;
    resp.status = 0;  // success -- bypass always admits
    resp.permitted_ts_ns = NowNanos();
    resp.ticket = NextBypassTicket();
    return resp;
}

void SchedulerProxy::StartReconnectProbe() {
    std::lock_guard<std::mutex> lock(reconnect_mutex_);
    if (reconnect_started_) return;
    reconnect_started_ = true;

    // Spawn a joinable background thread that periodically probes the
    // scheduler and transitions back to CONNECTED when it becomes available.
    reconnect_thread_ = std::thread([this]() {
        while (!stopped_.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(
                std::chrono::seconds(reconnect_interval_sec_));

            if (stopped_.load(std::memory_order_acquire)) return;

            if (ProbeScheduler()) {
                state_.store(State::CONNECTED, std::memory_order_release);
                consecutive_failures_.store(0, std::memory_order_relaxed);
                LOG(INFO) << "SchedulerProxy: reconnected to scheduler at "
                          << uds_path_;
                {
                    std::lock_guard<std::mutex> lk(reconnect_mutex_);
                    reconnect_started_ = false;
                }
                return;
            }
        }
    });
}

}  // namespace falconkv

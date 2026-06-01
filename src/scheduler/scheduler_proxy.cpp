#include "src/scheduler/scheduler_proxy.h"

#include <chrono>
#include <atomic>

#include "src/common/logging.h"

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

SchedulerProxy::SchedulerProxy(const std::string& uds_path)
    : uds_path_(uds_path),
      state_(State::DISCONNECTED) {
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
    // The reconnect thread (if any) is a detached thread, so there is nothing
    // to join.  If we wanted cleaner shutdown we could add a stop flag, but
    // the proxy is typically alive for the process lifetime.
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

    // state == CONNECTED -- try the RPC.
    // For now the RPC path is a placeholder.  When brpc is available this
    // would open a channel to uds_path_ and call RequestIO().
    // We simulate a successful RPC here.

    // Mock RPC: if the UDS file "conceptually" exists, succeed.
    // In reality we would issue a brpc call and check for errors.
    // For the mock implementation we just pretend the call succeeded.
    bool rpc_ok = true;  // placeholder -- always succeeds in mock mode.

    if (rpc_ok) {
        consecutive_failures_.store(0, std::memory_order_relaxed);
        IOResponseData resp;
        resp.status = 0;
        resp.permitted_ts_ns = NowNanos();
        resp.ticket = NextBypassTicket();  // mock: local ticket
        return resp;
    }

    // RPC failed.
    int failures = consecutive_failures_.fetch_add(1, std::memory_order_relaxed) + 1;
    if (failures >= MAX_CONSECUTIVE_FAILURES) {
        state_.store(State::BYPASS, std::memory_order_release);
        StartReconnectProbe();
    }
    return MakeBypassResponse(request);
}

void SchedulerProxy::ReportIOCompletion(const IOCompletionData& report) {
    State s = state_.load(std::memory_order_acquire);
    if (s == State::BYPASS) {
        // Silently drop -- scheduler is unreachable.
        return;
    }

    // Mock: pretend we sent the report.
    // In a real implementation this would be an RPC call.
    (void)report;
}

void SchedulerProxy::StoreReportIOAsync(uint32_t /*store_id*/,
                                        int /*io_channel*/,
                                        uint32_t /*source_client_id*/,
                                        uint64_t /*io_size*/,
                                        uint64_t /*request_ts_ns*/,
                                        uint64_t /*done_ts_ns*/,
                                        const std::string& /*source_node_addr*/) {
    // Fire-and-forget.  In the real implementation this would be an async RPC.
    // For the mock we simply do nothing.
}

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

bool SchedulerProxy::ProbeScheduler() {
    // In a real implementation we would try to connect to the UDS endpoint
    // (e.g. by creating a brpc Channel pointing at the Unix-domain socket).
    // For the mock we simply return false to indicate the scheduler is not
    // reachable.  Override to true if you want to test the connected path.
    return false;
}

void SchedulerProxy::StartReconnectProbe() {
    std::lock_guard<std::mutex> lock(reconnect_mutex_);
    if (reconnect_started_) return;
    reconnect_started_ = true;

    // Spawn a detached background thread that periodically probes the
    // scheduler and transitions back to CONNECTED when it becomes available.
    std::thread([this]() {
        while (true) {
            std::this_thread::sleep_for(
                std::chrono::seconds(RECONNECT_INTERVAL_SEC));

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
    }).detach();
}

}  // namespace falconkv

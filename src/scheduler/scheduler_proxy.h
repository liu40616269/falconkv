#pragma once
#include <string>
#include <atomic>
#include <thread>
#include <mutex>
#include "src/common/status.h"
#include "src/scheduler/passthrough_policy.h"

namespace falconkv {

// ---------------------------------------------------------------------------
// SchedulerProxy
//
// Client/Store-side proxy that communicates with FalconKVScheduler over a
// Unix-domain socket.  When the scheduler is unavailable the proxy enters
// *bypass mode* and locally admits every request so that IO can continue
// (albeit without central scheduling).
//
// The proxy automatically probes for the scheduler and reconnects when it
// comes back online.
// ---------------------------------------------------------------------------
class SchedulerProxy {
public:
    explicit SchedulerProxy(const std::string& uds_path);
    ~SchedulerProxy();

    /// Send an IO request to the scheduler and wait for permission.
    /// Returns a bypass response if the scheduler is unreachable.
    IOResponseData RequestIO(const IORequestData& request);

    /// Report IO completion to the scheduler (best-effort).
    void ReportIOCompletion(const IOCompletionData& report);

    /// Async report from a store node (fire-and-forget).
    void StoreReportIOAsync(uint32_t store_id,
                            int io_channel,
                            uint32_t source_client_id,
                            uint64_t io_size,
                            uint64_t request_ts_ns,
                            uint64_t done_ts_ns,
                            const std::string& source_node_addr);

    /// True when the proxy is operating in bypass mode.
    bool IsBypassMode() const;

private:
    enum class State { CONNECTED, DISCONNECTED, BYPASS };

    IOResponseData MakeBypassResponse(const IORequestData& request);
    void StartReconnectProbe();
    bool ProbeScheduler();

    std::string uds_path_;
    std::atomic<State> state_;
    std::atomic<int> consecutive_failures_{0};

    static constexpr int MAX_CONSECUTIVE_FAILURES = 3;
    static constexpr int RECONNECT_INTERVAL_SEC = 2;

    mutable std::mutex reconnect_mutex_;
    bool reconnect_started_ = false;
};

}  // namespace falconkv

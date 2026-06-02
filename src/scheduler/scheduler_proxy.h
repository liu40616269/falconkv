#pragma once
#include <string>
#include <atomic>
#include <thread>
#include <mutex>
#include <memory>
#include "src/common/status.h"
#include "src/scheduler/passthrough_policy.h"

#ifdef FALCONKV_HAS_BRPC
#include <brpc/channel.h>
#include "falconkv_scheduler.pb.h"
#endif

namespace falconkv {

// ---------------------------------------------------------------------------
// SchedulerProxy
//
// Client/Store-side proxy that communicates with FalconKVScheduler over a
// Unix-domain socket via brpc.  When the scheduler is unavailable the proxy
// enters *bypass mode* and locally admits every request so that IO can
// continue (albeit without central scheduling).
//
// The proxy automatically probes for the scheduler and reconnects when it
// comes back online.
// ---------------------------------------------------------------------------
class SchedulerProxy {
public:
    explicit SchedulerProxy(const std::string& uds_path,
                            int rpc_timeout_ms = 2,
                            int max_consecutive_failures = 3,
                            int reconnect_interval_sec = 2);
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

#ifdef FALCONKV_HAS_BRPC
    /// Establish a brpc channel to the scheduler over UDS.
    bool Connect();
#endif

    std::string uds_path_;
    std::atomic<State> state_;
    std::atomic<int> consecutive_failures_{0};
    std::atomic<bool> stopped_{false};

    int max_consecutive_failures_ = 3;
    int reconnect_interval_sec_ = 2;
    int rpc_timeout_ms_ = 2;

    mutable std::mutex reconnect_mutex_;
    bool reconnect_started_ = false;
    std::thread reconnect_thread_;

#ifdef FALCONKV_HAS_BRPC
    std::unique_ptr<brpc::Channel> channel_;
    std::unique_ptr<FalconKVSchedulerService_Stub> stub_;
#endif
};

}  // namespace falconkv

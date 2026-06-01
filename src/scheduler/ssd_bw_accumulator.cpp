#include "src/scheduler/ssd_bw_accumulator.h"

#include <algorithm>
#include <chrono>

namespace falconkv {

namespace {

uint64_t NowNanos() {
    auto tp = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(tp).count());
}

}  // namespace

// ---------------------------------------------------------------------------
// SSDBandwidthAccumulator
// ---------------------------------------------------------------------------

SSDBandwidthAccumulator::SSDBandwidthAccumulator(double ssd_bw_limit_mbps)
    : ssd_bw_limit_mbps_(ssd_bw_limit_mbps) {}

void SSDBandwidthAccumulator::OnIOStart(uint64_t ticket,
                                        uint64_t start_ts_ns,
                                        uint64_t io_size,
                                        int channel) {
    if (!ConsumesSSD(channel)) return;
    std::lock_guard<std::mutex> lock(mutex_);
    active_ios_.push_back({ticket, start_ts_ns, io_size, channel});
}

void SSDBandwidthAccumulator::OnIODone(uint64_t ticket,
                                       uint64_t done_ts_ns,
                                       uint64_t /*io_size*/) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Update peak while the IO is still active, so bandwidth is captured.
    UpdatePeakBandwidth(done_ts_ns);

    auto it = std::find_if(active_ios_.begin(), active_ios_.end(),
                           [ticket](const ActiveIO& aio) {
                               return aio.ticket == ticket;
                           });
    if (it != active_ios_.end()) {
        active_ios_.erase(it);
    }
}

double SSDBandwidthAccumulator::GetConcurrentBandwidthMBps(uint64_t now_ns) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return GetConcurrentBandwidthMBpsLocked(now_ns);
}

bool SSDBandwidthAccumulator::IsSSDBandwidthSaturated() const {
    if (ssd_bw_limit_mbps_ <= 0.0) return false;
    return GetConcurrentBandwidthMBps(NowNanos()) >=
           ssd_bw_limit_mbps_ * saturation_threshold_;
}

void SSDBandwidthAccumulator::UpdatePeakBandwidth(uint64_t ts_ns) {
    double bw = GetConcurrentBandwidthMBpsLocked(ts_ns);
    if (bw > peak_bandwidth_mbps_) {
        peak_bandwidth_mbps_ = bw;
    }
}

double SSDBandwidthAccumulator::GetConcurrentBandwidthMBpsLocked(uint64_t now_ns) const {
    if (active_ios_.empty()) return 0.0;

    uint64_t total_bytes = 0;
    for (const auto& aio : active_ios_) {
        total_bytes += aio.io_size;
    }

    uint64_t earliest = active_ios_.front().start_ts_ns;
    for (const auto& aio : active_ios_) {
        if (aio.start_ts_ns < earliest) earliest = aio.start_ts_ns;
    }

    uint64_t elapsed = now_ns > earliest ? (now_ns - earliest) : 1;
    return (static_cast<double>(total_bytes) / (1024.0 * 1024.0))
         / (static_cast<double>(elapsed) / 1e9);
}

}  // namespace falconkv

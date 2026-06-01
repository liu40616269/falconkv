#include "src/scheduler/net_bw_accumulator.h"

#include <algorithm>
#include <chrono>

namespace falconkv {

namespace {

uint64_t NowNanos() {
    auto tp = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(tp).count());
}

double ComputeBandwidthMBps(const std::vector<NetBandwidthAccumulator::ActiveIO>& ios,
                            uint64_t now_ns,
                            int channel_filter,        // -1 = all
                            const std::string* addr_filter) { // nullptr = all
    if (ios.empty()) return 0.0;

    uint64_t total_bytes = 0;
    uint64_t earliest = UINT64_MAX;

    for (const auto& aio : ios) {
        if (channel_filter >= 0 && aio.channel != channel_filter) continue;
        if (addr_filter && !addr_filter->empty() && aio.node_addr != *addr_filter) continue;
        total_bytes += aio.io_size;
        if (aio.start_ts_ns < earliest) earliest = aio.start_ts_ns;
    }

    if (total_bytes == 0) return 0.0;
    uint64_t elapsed = now_ns > earliest ? (now_ns - earliest) : 1;
    return (static_cast<double>(total_bytes) / (1024.0 * 1024.0))
         / (static_cast<double>(elapsed) / 1e9);
}

}  // namespace

// ---------------------------------------------------------------------------
// NetBandwidthAccumulator
// ---------------------------------------------------------------------------

NetBandwidthAccumulator::NetBandwidthAccumulator(double net_bw_limit_mbps)
    : net_bw_limit_mbps_(net_bw_limit_mbps) {}

void NetBandwidthAccumulator::OnIOStart(uint64_t ticket,
                                        uint64_t start_ts_ns,
                                        uint64_t io_size,
                                        int channel,
                                        const std::string& node_addr) {
    // Only track network channels.
    if (!IsTX(channel) && !IsRX(channel)) return;
    std::lock_guard<std::mutex> lock(mutex_);
    active_ios_.push_back({ticket, start_ts_ns, io_size, channel, node_addr});
}

void NetBandwidthAccumulator::OnIODone(uint64_t ticket) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Update peak while the IO is still active.
    UpdatePeakBandwidth(NowNanos());

    auto it = std::find_if(active_ios_.begin(), active_ios_.end(),
                           [ticket](const ActiveIO& aio) {
                               return aio.ticket == ticket;
                           });
    if (it != active_ios_.end()) {
        active_ios_.erase(it);
    }
}

double NetBandwidthAccumulator::GetNetTxBandwidthMBps(uint64_t now_ns) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return GetNetTxBandwidthMBpsLocked(now_ns);
}

double NetBandwidthAccumulator::GetNetRxBandwidthMBps(uint64_t now_ns) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return GetNetRxBandwidthMBpsLocked(now_ns);
}

double NetBandwidthAccumulator::GetNetTxBandwidthToNodeMBps(
        uint64_t now_ns, const std::string& node_addr) const {
    std::lock_guard<std::mutex> lock(mutex_);
    double bw = 0.0;
    bw += ComputeBandwidthMBps(active_ios_, now_ns, 2, &node_addr);
    return bw;
}

double NetBandwidthAccumulator::GetNetRxBandwidthFromNodeMBps(
        uint64_t now_ns, const std::string& node_addr) const {
    std::lock_guard<std::mutex> lock(mutex_);
    double bw = 0.0;
    bw += ComputeBandwidthMBps(active_ios_, now_ns, 3, &node_addr);
    return bw;
}

bool NetBandwidthAccumulator::IsNetTxBandwidthSaturated() const {
    if (net_bw_limit_mbps_ <= 0.0) return false;
    return GetNetTxBandwidthMBps(NowNanos()) >=
           net_bw_limit_mbps_ * saturation_threshold_;
}

bool NetBandwidthAccumulator::IsNetRxBandwidthSaturated() const {
    if (net_bw_limit_mbps_ <= 0.0) return false;
    return GetNetRxBandwidthMBps(NowNanos()) >=
           net_bw_limit_mbps_ * saturation_threshold_;
}

void NetBandwidthAccumulator::UpdatePeakBandwidth(uint64_t ts_ns) {
    double tx = GetNetTxBandwidthMBpsLocked(ts_ns);
    double rx = GetNetRxBandwidthMBpsLocked(ts_ns);
    if (tx > peak_net_tx_mbps_) peak_net_tx_mbps_ = tx;
    if (rx > peak_net_rx_mbps_) peak_net_rx_mbps_ = rx;
}

double NetBandwidthAccumulator::GetNetTxBandwidthMBpsLocked(uint64_t now_ns) const {
    double bw = 0.0;
    bw += ComputeBandwidthMBps(active_ios_, now_ns, 2, nullptr);  // NET_TX_READ
    return bw;
}

double NetBandwidthAccumulator::GetNetRxBandwidthMBpsLocked(uint64_t now_ns) const {
    double bw = 0.0;
    bw += ComputeBandwidthMBps(active_ios_, now_ns, 3, nullptr);  // NET_RX_READ
    return bw;
}

}  // namespace falconkv

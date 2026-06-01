#pragma once
#include <vector>
#include <mutex>
#include <cstdint>
#include "src/scheduler/passthrough_policy.h"

namespace falconkv {

// ---------------------------------------------------------------------------
// SSDBandwidthAccumulator
//
// Tracks the aggregate bandwidth of all *concurrent* IOs that consume SSD
// resources (local SSD read/write and network RX read/write).  This is used
// by the scheduler to decide whether the SSD is approaching saturation.
// ---------------------------------------------------------------------------
class SSDBandwidthAccumulator {
public:
    explicit SSDBandwidthAccumulator(double ssd_bw_limit_mbps = 7000.0);

    /// Called when a new IO starts.
    void OnIOStart(uint64_t ticket, uint64_t start_ts_ns,
                   uint64_t io_size, int channel);

    /// Called when an IO finishes.
    void OnIODone(uint64_t ticket, uint64_t done_ts_ns, uint64_t io_size);

    /// Estimate current aggregate SSD bandwidth based on active IOs.
    double GetConcurrentBandwidthMBps(uint64_t now_ns) const;

    /// True when utilisation exceeds the saturation threshold.
    bool IsSSDBandwidthSaturated() const;

    /// Highest observed concurrent bandwidth.
    double GetPeakBandwidthMBps() const { return peak_bandwidth_mbps_; }

private:
    static bool ConsumesSSD(int channel) {
        // LOCAL_SSD_READ=0, LOCAL_SSD_WRITE=1, NET_RX_READ=3
        return channel == 0 || channel == 1 || channel == 3;
    }

    void UpdatePeakBandwidth(uint64_t ts_ns);

    // Must be called with mutex_ held.
    double GetConcurrentBandwidthMBpsLocked(uint64_t now_ns) const;

    struct ActiveIO {
        uint64_t ticket;
        uint64_t start_ts_ns;
        uint64_t io_size;
        int channel;
    };

    mutable std::mutex mutex_;
    std::vector<ActiveIO> active_ios_;
    double ssd_bw_limit_mbps_;
    double saturation_threshold_ = 0.9;
    double peak_bandwidth_mbps_ = 0.0;
};

}  // namespace falconkv

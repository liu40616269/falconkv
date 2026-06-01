#pragma once
#include <vector>
#include <mutex>
#include <string>
#include <cstdint>

namespace falconkv {

// ---------------------------------------------------------------------------
// NetBandwidthAccumulator
//
// Tracks concurrent network bandwidth consumption for both TX and RX
// directions, with optional per-node breakdown.  Used by the scheduler to
// detect network saturation.
// ---------------------------------------------------------------------------
class NetBandwidthAccumulator {
public:
    explicit NetBandwidthAccumulator(double net_bw_limit_mbps = 12500.0);

    /// Called when a network IO starts.
    void OnIOStart(uint64_t ticket, uint64_t start_ts_ns,
                   uint64_t io_size, int channel,
                   const std::string& node_addr);

    /// Called when a network IO finishes (matched by ticket).
    void OnIODone(uint64_t ticket);

    /// Aggregate TX bandwidth across all active network TX IOs.
    double GetNetTxBandwidthMBps(uint64_t now_ns) const;

    /// Aggregate RX bandwidth across all active network RX IOs.
    double GetNetRxBandwidthMBps(uint64_t now_ns) const;

    /// TX bandwidth to a specific node.
    double GetNetTxBandwidthToNodeMBps(uint64_t now_ns,
                                       const std::string& node_addr) const;

    /// RX bandwidth from a specific node.
    double GetNetRxBandwidthFromNodeMBps(uint64_t now_ns,
                                         const std::string& node_addr) const;

    /// True when TX utilisation exceeds the saturation threshold.
    bool IsNetTxBandwidthSaturated() const;

    /// True when RX utilisation exceeds the saturation threshold.
    bool IsNetRxBandwidthSaturated() const;

    double GetPeakNetTxBandwidthMBps() const { return peak_net_tx_mbps_; }
    double GetPeakNetRxBandwidthMBps() const { return peak_net_rx_mbps_; }

    struct ActiveIO {
        uint64_t ticket;
        uint64_t start_ts_ns;
        uint64_t io_size;
        int channel;
        std::string node_addr;
    };

private:
    void UpdatePeakBandwidth(uint64_t ts_ns);

    // Must be called with mutex_ held.
    double GetNetTxBandwidthMBpsLocked(uint64_t now_ns) const;
    double GetNetRxBandwidthMBpsLocked(uint64_t now_ns) const;

    static bool IsTX(int channel) {
        // NET_TX_READ=2
        return channel == 2;
    }

    static bool IsRX(int channel) {
        // NET_RX_READ=3
        return channel == 3;
    }

    mutable std::mutex mutex_;
    std::vector<ActiveIO> active_ios_;
    double net_bw_limit_mbps_;
    double saturation_threshold_ = 0.9;
    double peak_net_tx_mbps_ = 0.0;
    double peak_net_rx_mbps_ = 0.0;
};

}  // namespace falconkv

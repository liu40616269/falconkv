#include <gtest/gtest.h>
#include <chrono>
#include <string>

#include "src/scheduler/net_bw_accumulator.h"

using namespace falconkv;

namespace {

uint64_t NowNanos() {
    auto tp = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(tp).count());
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Per-node-addr channel stats
// ---------------------------------------------------------------------------
TEST(NetBandwidthAccumulator, PerNodeAddrStats) {
    NetBandwidthAccumulator acc(12500.0);
    uint64_t now = NowNanos();

    acc.OnIOStart(1, now, 10 * 1024 * 1024, 2, "10.0.0.2:8901"); // NET_TX_READ

    double bw = acc.GetNetTxBandwidthToNodeMBps(now + 1000000, "10.0.0.2:8901");
    EXPECT_GT(bw, 0.0);

    // Different node should have zero bandwidth.
    double bw_other = acc.GetNetTxBandwidthToNodeMBps(now + 1000000, "10.0.0.3:8901");
    EXPECT_DOUBLE_EQ(bw_other, 0.0);
}

// ---------------------------------------------------------------------------
// TX/RX independence
// ---------------------------------------------------------------------------
TEST(NetBandwidthAccumulator, TXAndRXIndependence) {
    NetBandwidthAccumulator acc(12500.0);
    uint64_t now = NowNanos();

    // Only TX IO active.
    acc.OnIOStart(1, now, 10 * 1024 * 1024, 2, "10.0.0.2:8901"); // NET_TX_READ

    double tx_bw = acc.GetNetTxBandwidthMBps(now + 1000000);
    double rx_bw = acc.GetNetRxBandwidthMBps(now + 1000000);

    EXPECT_GT(tx_bw, 0.0);
    EXPECT_DOUBLE_EQ(rx_bw, 0.0);
}

TEST(NetBandwidthAccumulator, RXOnlyActive) {
    NetBandwidthAccumulator acc(12500.0);
    uint64_t now = NowNanos();

    // Only RX IO active.
    acc.OnIOStart(1, now, 20 * 1024 * 1024, 3, "10.0.0.3:8901"); // NET_RX_READ

    double tx_bw = acc.GetNetTxBandwidthMBps(now + 1000000);
    double rx_bw = acc.GetNetRxBandwidthMBps(now + 1000000);

    EXPECT_DOUBLE_EQ(tx_bw, 0.0);
    EXPECT_GT(rx_bw, 0.0);
}

// ---------------------------------------------------------------------------
// Per-node bandwidth query
// ---------------------------------------------------------------------------
TEST(NetBandwidthAccumulator, PerNodeTXBandwidth) {
    NetBandwidthAccumulator acc(12500.0);
    uint64_t now = NowNanos();

    acc.OnIOStart(1, now, 5 * 1024 * 1024, 2, "10.0.0.1:8901");
    acc.OnIOStart(2, now, 15 * 1024 * 1024, 2, "10.0.0.2:8901");

    double bw1 = acc.GetNetTxBandwidthToNodeMBps(now + 1000000, "10.0.0.1:8901");
    double bw2 = acc.GetNetTxBandwidthToNodeMBps(now + 1000000, "10.0.0.2:8901");

    EXPECT_GT(bw1, 0.0);
    EXPECT_GT(bw2, 0.0);
}

TEST(NetBandwidthAccumulator, PerNodeRXBandwidth) {
    NetBandwidthAccumulator acc(12500.0);
    uint64_t now = NowNanos();

    acc.OnIOStart(1, now, 10 * 1024 * 1024, 3, "10.0.0.1:8901"); // NET_RX_READ

    double bw = acc.GetNetRxBandwidthFromNodeMBps(now + 1000000, "10.0.0.1:8901");
    EXPECT_GT(bw, 0.0);

    // A different node should have 0 bandwidth.
    double bw2 = acc.GetNetRxBandwidthFromNodeMBps(now + 1000000, "10.0.0.99:8901");
    EXPECT_DOUBLE_EQ(bw2, 0.0);
}

// ---------------------------------------------------------------------------
// OnIODone removes entry
// ---------------------------------------------------------------------------
TEST(NetBandwidthAccumulator, OnIODoneRemovesEntry) {
    NetBandwidthAccumulator acc(12500.0);
    uint64_t now = NowNanos();

    acc.OnIOStart(1, now, 10 * 1024 * 1024, 2, "10.0.0.1:8901");
    EXPECT_GT(acc.GetNetTxBandwidthMBps(now + 1000), 0.0);

    acc.OnIODone(1);

    double bw = acc.GetNetTxBandwidthMBps(now + 2000000);
    EXPECT_DOUBLE_EQ(bw, 0.0);
}

// ---------------------------------------------------------------------------
// Local SSD channels are not tracked
// ---------------------------------------------------------------------------
TEST(NetBandwidthAccumulator, LocalSSDChannelsNotTracked) {
    NetBandwidthAccumulator acc(12500.0);
    uint64_t now = NowNanos();

    // LOCAL_SSD_READ (0) and LOCAL_SSD_WRITE (1) are not network channels.
    acc.OnIOStart(1, now, 100 * 1024 * 1024, 0, "");
    acc.OnIOStart(2, now, 100 * 1024 * 1024, 1, "");

    double tx_bw = acc.GetNetTxBandwidthMBps(now + 1000000);
    double rx_bw = acc.GetNetRxBandwidthMBps(now + 1000000);

    EXPECT_DOUBLE_EQ(tx_bw, 0.0);
    EXPECT_DOUBLE_EQ(rx_bw, 0.0);
}

// ---------------------------------------------------------------------------
// Peak bandwidth tracking
// ---------------------------------------------------------------------------
TEST(NetBandwidthAccumulator, PeakBandwidthTracking) {
    NetBandwidthAccumulator acc(12500.0);
    uint64_t now = NowNanos();

    EXPECT_DOUBLE_EQ(acc.GetPeakNetTxBandwidthMBps(), 0.0);
    EXPECT_DOUBLE_EQ(acc.GetPeakNetRxBandwidthMBps(), 0.0);

    acc.OnIOStart(1, now, 50 * 1024 * 1024, 2, "10.0.0.1:8901");
    acc.OnIODone(1);

    EXPECT_GT(acc.GetPeakNetTxBandwidthMBps(), 0.0);
}

TEST(NetBandwidthAccumulator, NotSaturatedByDefault) {
    NetBandwidthAccumulator acc(12500.0);
    EXPECT_FALSE(acc.IsNetTxBandwidthSaturated());
    EXPECT_FALSE(acc.IsNetRxBandwidthSaturated());
}

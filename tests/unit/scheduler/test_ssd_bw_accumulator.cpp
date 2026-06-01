#include <gtest/gtest.h>
#include <chrono>

#include "src/scheduler/ssd_bw_accumulator.h"

using namespace falconkv;

namespace {

uint64_t NowNanos() {
    auto tp = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(tp).count());
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// SSD bandwidth accumulation
// ---------------------------------------------------------------------------
TEST(SSDBandwidthAccumulator, OnIOStartTracksSSDChannels) {
    SSDBandwidthAccumulator acc(7000.0);
    uint64_t now = NowNanos();

    // LOCAL_SSD_READ (channel 0) -- consumes SSD.
    acc.OnIOStart(1, now, 1024 * 1024, 0);

    double bw = acc.GetConcurrentBandwidthMBps(now + 1000000);
    EXPECT_GT(bw, 0.0);
}

TEST(SSDBandwidthAccumulator, MultipleSSDIOsAccumulate) {
    SSDBandwidthAccumulator acc(7000.0);
    uint64_t now = NowNanos();

    acc.OnIOStart(1, now, 10 * 1024 * 1024, 0);  // LOCAL_SSD_READ
    acc.OnIOStart(2, now, 20 * 1024 * 1024, 1);  // LOCAL_SSD_WRITE
    acc.OnIOStart(3, now, 30 * 1024 * 1024, 3);  // NET_RX_READ

    double bw = acc.GetConcurrentBandwidthMBps(now + 1000000);
    // All three channels consume SSD.  Total ~ 60 MB over 1ms = ~60,000 MB/s.
    EXPECT_GT(bw, 0.0);
}

// ---------------------------------------------------------------------------
// Non-SSD channel exclusion
// ---------------------------------------------------------------------------
TEST(SSDBandwidthAccumulator, NonSSDChannelsExcluded) {
    SSDBandwidthAccumulator acc(7000.0);
    uint64_t now = NowNanos();

    // NET_TX_READ (channel 2) -- does NOT consume SSD.
    acc.OnIOStart(1, now, 100 * 1024 * 1024, 2);

    double bw = acc.GetConcurrentBandwidthMBps(now + 1000000);
    EXPECT_DOUBLE_EQ(bw, 0.0);
}

// ---------------------------------------------------------------------------
// OnIODone removes entry
// ---------------------------------------------------------------------------
TEST(SSDBandwidthAccumulator, OnIODoneRemovesEntry) {
    SSDBandwidthAccumulator acc(7000.0);
    uint64_t now = NowNanos();

    acc.OnIOStart(1, now, 10 * 1024 * 1024, 0);
    EXPECT_GT(acc.GetConcurrentBandwidthMBps(now + 1000), 0.0);

    acc.OnIODone(1, now + 1000000, 10 * 1024 * 1024);

    // After IO completion, bandwidth should be 0 (no active IOs).
    double bw = acc.GetConcurrentBandwidthMBps(now + 2000000);
    EXPECT_DOUBLE_EQ(bw, 0.0);
}

// ---------------------------------------------------------------------------
// Peak bandwidth tracking
// ---------------------------------------------------------------------------
TEST(SSDBandwidthAccumulator, PeakBandwidthTracking) {
    SSDBandwidthAccumulator acc(7000.0);
    uint64_t now = NowNanos();

    EXPECT_DOUBLE_EQ(acc.GetPeakBandwidthMBps(), 0.0);

    acc.OnIOStart(1, now, 100 * 1024 * 1024, 0);
    acc.OnIODone(1, now + 1000000, 100 * 1024 * 1024);

    // Peak should have been updated during the IO lifetime.
    EXPECT_GT(acc.GetPeakBandwidthMBps(), 0.0);
}

TEST(SSDBandwidthAccumulator, IsSSDBandwidthSaturatedFalse) {
    SSDBandwidthAccumulator acc(7000.0);
    // With no IOs, should not be saturated.
    EXPECT_FALSE(acc.IsSSDBandwidthSaturated());
}

#include <gtest/gtest.h>
#include <string>
#include <chrono>

#include "src/scheduler/scheduler_proxy.h"

using namespace falconkv;

// ---------------------------------------------------------------------------
// Initial state is disconnected/bypass
// ---------------------------------------------------------------------------
TEST(SchedulerProxyBypass, InitialStateIsDisconnected) {
    // The probe will fail (mock always returns false), so the proxy starts
    // in DISCONNECTED state, which causes it to fall back to bypass.
    SchedulerProxy proxy("/tmp/falconkv_test_scheduler_nonexistent.sock");

    // Since ProbeScheduler() returns false in the mock, and there's no real
    // scheduler to connect to, the proxy should be in bypass mode.
    EXPECT_TRUE(proxy.IsBypassMode());
}

TEST(SchedulerProxyBypass, InitialStateNotConnected) {
    SchedulerProxy proxy("/tmp/nonexistent_uds_path.sock");
    // After construction, probe fails so it transitions to BYPASS.
    EXPECT_TRUE(proxy.IsBypassMode());
}

// ---------------------------------------------------------------------------
// RequestIO returns bypass response
// ---------------------------------------------------------------------------
TEST(SchedulerProxyBypass, RequestIOReturnsBypassResponse) {
    SchedulerProxy proxy("/tmp/falconkv_test_nonexistent.sock");

    IORequestData request;
    request.client_id = 1;
    request.io_channel = 0;
    request.store_id = 1;
    request.io_size = 4096;

    IOResponseData response = proxy.RequestIO(request);

    // Bypass always admits.
    EXPECT_EQ(response.status, 0);
    // Bypass tickets have the high bit set.
    EXPECT_NE(response.ticket, 0u);
    EXPECT_GT(response.permitted_ts_ns, 0u);
}

TEST(SchedulerProxyBypass, MultipleBypassRequests) {
    SchedulerProxy proxy("/tmp/falconkv_test_nonexistent.sock");

    IORequestData request;

    uint64_t prev_ticket = 0;
    for (int i = 0; i < 10; ++i) {
        IOResponseData response = proxy.RequestIO(request);
        EXPECT_EQ(response.status, 0);
        if (i > 0) {
            EXPECT_NE(response.ticket, prev_ticket);
        }
        prev_ticket = response.ticket;
    }
}

// ---------------------------------------------------------------------------
// IsBypassMode detection
// ---------------------------------------------------------------------------
TEST(SchedulerProxyBypass, IsBypassModeReturnsTrue) {
    SchedulerProxy proxy("/tmp/falconkv_test_nonexistent.sock");
    EXPECT_TRUE(proxy.IsBypassMode());
}

// ---------------------------------------------------------------------------
// ReportIOCompletion in bypass mode is silently dropped
// ---------------------------------------------------------------------------
TEST(SchedulerProxyBypass, ReportIOCompletionInBypassNoop) {
    SchedulerProxy proxy("/tmp/falconkv_test_nonexistent.sock");

    IOCompletionData report;
    report.client_id = 1;
    report.ticket = 42;
    report.io_size = 4096;

    // Should not crash or hang.
    proxy.ReportIOCompletion(report);
}

// ---------------------------------------------------------------------------
// StoreReportIOAsync in bypass mode is silently dropped
// ---------------------------------------------------------------------------
TEST(SchedulerProxyBypass, StoreReportIOAsyncNoop) {
    SchedulerProxy proxy("/tmp/falconkv_test_nonexistent.sock");

    // Should not crash or hang.
    proxy.StoreReportIOAsync(1, 0, 1, 4096, 0, 1000, "10.0.0.1:8901");
}

// ---------------------------------------------------------------------------
// Consecutive probe failures cause bypass mode
// ---------------------------------------------------------------------------
TEST(SchedulerProxyBypass, ConsecutiveFailureTriggersBypass) {
    // Connecting to a nonexistent UDS will fail all probes.
    SchedulerProxy proxy("/tmp/falconkv_test_consecutive_fail.sock");

    // After construction, the probe should have failed, so bypass mode.
    EXPECT_TRUE(proxy.IsBypassMode());

    // Multiple RequestIO calls should all succeed in bypass mode.
    IORequestData request;
    for (int i = 0; i < 10; ++i) {
        IOResponseData resp = proxy.RequestIO(request);
        EXPECT_EQ(resp.status, 0) << "Bypass request " << i << " failed";
    }

    // Should still be in bypass mode after many requests.
    EXPECT_TRUE(proxy.IsBypassMode());
}

// ---------------------------------------------------------------------------
// Reconnect probe thread does not crash in bypass mode
// ---------------------------------------------------------------------------
TEST(SchedulerProxyBypass, ReconnectProbeDoesNotCrash) {
    SchedulerProxy proxy("/tmp/falconkv_test_reconnect.sock");

    EXPECT_TRUE(proxy.IsBypassMode());

    // Trigger several operations that may cause reconnect probes internally.
    IORequestData request;
    for (int i = 0; i < 5; ++i) {
        proxy.RequestIO(request);
        proxy.ReportIOCompletion(IOCompletionData{});
        proxy.StoreReportIOAsync(1, 0, 1, 4096, 0, 1000, "10.0.0.1:8901");
    }

    // If we reach here without crashing, the test passes.
    EXPECT_TRUE(proxy.IsBypassMode());
}

// ---------------------------------------------------------------------------
// Single bypass RequestIO latency is sub-100us
// ---------------------------------------------------------------------------
TEST(SchedulerProxyBypass, BypassResponseLatency) {
    SchedulerProxy proxy("/tmp/falconkv_test_latency.sock");

    IORequestData request;
    request.client_id = 1;
    request.io_size = 4096;

    auto start = std::chrono::steady_clock::now();
    IOResponseData resp = proxy.RequestIO(request);
    auto end = std::chrono::steady_clock::now();

    EXPECT_EQ(resp.status, 0);

    auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    EXPECT_LT(elapsed_us, 100) << "Bypass RequestIO took " << elapsed_us << "us, expected < 100us";
}

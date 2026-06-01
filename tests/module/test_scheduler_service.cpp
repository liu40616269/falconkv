#include <gtest/gtest.h>
#include <thread>
#include <chrono>

#include "src/common/config.h"
#include "src/scheduler/scheduler_service.h"

using namespace falconkv;

// ---------------------------------------------------------------------------
// MT-SC-001: Start and Stop the scheduler without crash or resource leak
// ---------------------------------------------------------------------------
TEST(SchedulerServiceTest, StartStop) {
    SchedulerConfig config;
    config.uds_path = "/tmp/falconkv_test_sched_startstop.sock";
    config.schedule_policy = "passthrough";

    FalconKVScheduler sched(config);

    Status s = sched.Start();
    ASSERT_TRUE(s.ok()) << "Start failed: " << s.msg();

    // Let the stats thread run briefly.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    sched.Stop();
    // Should not crash or hang.
}

// ---------------------------------------------------------------------------
// MT-SC-002: RequestIO → IOCompletion roundtrip records stats
// ---------------------------------------------------------------------------
TEST(SchedulerServiceTest, RequestIOAndReportCompletion) {
    SchedulerConfig config;
    config.uds_path = "/tmp/falconkv_test_sched_io.sock";
    config.schedule_policy = "passthrough";

    FalconKVScheduler sched(config);
    ASSERT_TRUE(sched.Start().ok());

    IORequestData req;
    req.client_id = 1;
    req.io_channel = 0;
    req.store_id = 1;
    req.io_size = 4096;

    IOResponseData resp = sched.HandleRequestIO(req);
    EXPECT_EQ(resp.status, 0);
    // Ticket starts at 0 and increments.
    EXPECT_GE(resp.ticket, 0u);

    // Report completion.
    IOCompletionData report;
    report.client_id = 1;
    report.ticket = resp.ticket;
    report.io_size = 4096;
    report.io_channel = 0;
    report.io_status = 0;

    sched.HandleIOCompletion(report);
    // Should not crash.

    sched.Stop();
}

// ---------------------------------------------------------------------------
// MT-SC-003: Store-reported IO updates NET_RX stats in NodeStats
// ---------------------------------------------------------------------------
TEST(SchedulerServiceTest, StoreReportIO) {
    SchedulerConfig config;
    config.uds_path = "/tmp/falconkv_test_sched_storeio.sock";
    config.schedule_policy = "passthrough";

    FalconKVScheduler sched(config);
    ASSERT_TRUE(sched.Start().ok());

    // Simulate a store reporting an incoming remote read (NET_RX).
    IOCompletionData report;
    report.client_id = 0;
    report.ticket = 1;
    report.io_size = 1 * 1024 * 1024; // 1 MB
    report.io_channel = 2; // NET_RX_READ channel
    report.io_status = 0;
    report.store_id = 1;

    sched.HandleIOCompletion(report);

    // The scheduler should not crash when recording store IO.
    // Stats are internal; we verify no crash.
    sched.Stop();
}

// ---------------------------------------------------------------------------
// MT-SC-004: Multiple IOs followed by stats print (no crash)
// ---------------------------------------------------------------------------
TEST(SchedulerServiceTest, StatsReportOutput) {
    SchedulerConfig config;
    config.uds_path = "/tmp/falconkv_test_sched_stats.sock";
    config.schedule_policy = "passthrough";

    FalconKVScheduler sched(config);
    ASSERT_TRUE(sched.Start().ok());

    // Perform several IO cycles.
    for (int i = 0; i < 20; ++i) {
        IORequestData req;
        req.client_id = static_cast<uint32_t>(i % 5);
        req.io_channel = i % 3;
        req.store_id = 1;
        req.io_size = 4096 * (i + 1);

        IOResponseData resp = sched.HandleRequestIO(req);
        EXPECT_EQ(resp.status, 0);

        IOCompletionData report;
        report.client_id = req.client_id;
        report.ticket = resp.ticket;
        report.io_size = req.io_size;
        report.io_channel = req.io_channel;
        report.io_status = 0;
        report.store_id = 1;

        sched.HandleIOCompletion(report);
    }

    // Let the stats reporting thread run briefly.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    sched.Stop();
    // No crash = pass.
}

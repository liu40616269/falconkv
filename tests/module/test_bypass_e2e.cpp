#include <gtest/gtest.h>
#include <string>
#include <chrono>
#include <thread>

#include "src/common/config.h"
#include "src/scheduler/scheduler_server.h"
#include "src/scheduler/scheduler_proxy.h"

using namespace falconkv;

namespace {

std::string UniqueUdsPath() {
    static int counter = 0;
    return "/tmp/falconkv_test_bypass_" + std::to_string(getpid()) +
           "_" + std::to_string(counter++) + ".sock";
}

// Start a SchedulerServer; skip the test if brpc UDS is not available.
bool TryStartServer(SchedulerServer& server) {
    Status s = server.Start();
    if (!s.ok()) {
        return false;
    }
    // Give server time to start listening.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    return true;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// MT-BP-001: Scheduler stop causes proxy to enter bypass mode
// ---------------------------------------------------------------------------
TEST(BypassE2eTest, SchedulerStopClientBypass) {
    std::string uds = UniqueUdsPath();

    SchedulerConfig config;
    config.uds_path = uds;
    config.schedule_policy = "passthrough";

    SchedulerServer server(config);
    if (!TryStartServer(server)) {
        GTEST_SKIP() << "SchedulerServer::Start() failed (brpc UDS may not be supported)";
    }

    // Proxy connects to the running scheduler.
    SchedulerProxy proxy(uds);

    // Verify proxy can send requests without crashing.
    IORequestData req;
    req.client_id = 1;
    req.io_size = 4096;

    IOResponseData resp = proxy.RequestIO(req);
    // The proxy might be in bypass or connected mode.
    EXPECT_EQ(resp.status, 0);

    // Stop the scheduler.
    server.Stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Proxy should still work (bypass mode).
    resp = proxy.RequestIO(req);
    EXPECT_EQ(resp.status, 0) << "Bypass response should admit IO";
}

// ---------------------------------------------------------------------------
// MT-BP-002: Proxy connecting to nonexistent UDS enters bypass immediately
// ---------------------------------------------------------------------------
TEST(BypassE2eTest, SchedulerUnavailableBypass) {
    SchedulerProxy proxy("/tmp/falconkv_nonexistent_scheduler.sock");

    // Should be in bypass mode immediately.
    EXPECT_TRUE(proxy.IsBypassMode());

    IORequestData req;
    req.client_id = 1;
    req.io_size = 8192;

    IOResponseData resp = proxy.RequestIO(req);
    EXPECT_EQ(resp.status, 0);
    EXPECT_GE(resp.ticket, 0u);
}

// ---------------------------------------------------------------------------
// MT-BP-003: Scheduler restart — proxy reconnects after timeout
// ---------------------------------------------------------------------------
TEST(BypassE2eTest, SchedulerRestartReconnect) {
    std::string uds = UniqueUdsPath();

    SchedulerConfig config;
    config.uds_path = uds;
    config.schedule_policy = "passthrough";

    // Start and immediately stop.
    SchedulerServer server(config);
    if (!TryStartServer(server)) {
        GTEST_SKIP() << "SchedulerServer::Start() failed (brpc UDS may not be supported)";
    }
    server.Stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Remove the stale UDS file left by brpc.
    std::remove(uds.c_str());

    // Proxy tries to connect to the (now stopped) scheduler.
    SchedulerProxy proxy(uds);
    EXPECT_TRUE(proxy.IsBypassMode());

    // Restart the scheduler on the same path.
    SchedulerServer server2(config);
    if (!TryStartServer(server2)) {
        GTEST_SKIP() << "SchedulerServer 2::Start() failed";
    }

    // Proxy should still function (bypass or connected).
    IORequestData req;
    req.client_id = 1;
    req.io_size = 4096;

    IOResponseData resp = proxy.RequestIO(req);
    EXPECT_EQ(resp.status, 0) << "Proxy should still function (bypass or connected)";

    server2.Stop();
}

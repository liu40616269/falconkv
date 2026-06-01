#include <gtest/gtest.h>
#include <chrono>
#include <thread>

#include "src/transfer/transfer_manager.h"
#include "src/transfer/retry_policy.h"
#include "src/common/status.h"
#include "src/common/config.h"

using namespace falconkv;

// ---------------------------------------------------------------------------
// MT-T-001: RetryPolicy retries on retriable errors and returns final status
// ---------------------------------------------------------------------------
TEST(TransferBrpcTest, RetryPolicy) {
    RetryPolicy policy(3, 50); // max 3 retries, 50ms base delay

    int call_count = 0;
    Status result = policy.ExecuteWithRetry([&]() {
        call_count++;
        return Status::RpcError("simulated failure");
    });

    // Should have retried 3 times (initial + 3 retries = 4 calls).
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(call_count, 4) << "Expected 4 calls (1 initial + 3 retries)";
}

// ---------------------------------------------------------------------------
// MT-T-002: RetryPolicy succeeds immediately on first OK
// ---------------------------------------------------------------------------
TEST(TransferBrpcTest, RetryPolicySuccessOnFirstTry) {
    RetryPolicy policy(3, 50);

    int call_count = 0;
    Status result = policy.ExecuteWithRetry([&]() {
        call_count++;
        return Status::OK();
    });

    EXPECT_TRUE(result.ok());
    EXPECT_EQ(call_count, 1) << "Should only call once on success";
}

// ---------------------------------------------------------------------------
// MT-T-003: TransferManager GetStoreChannel returns nullptr for unknown store
// ---------------------------------------------------------------------------
TEST(TransferBrpcTest, TransferManagerUnknownStore) {
    TransferConfig config;
    config.protocol = "brpc";

    TransferManager mgr(config);

    // Before registration, GetStoreChannel returns nullptr.
    TransferChannel* ch = mgr.GetStoreChannel(99);
    EXPECT_EQ(ch, nullptr);
}

// ---------------------------------------------------------------------------
// MT-T-004: TransferManager RegisterStoreAddr records the address
//           GetStoreChannel attempts connection (may fail without a server)
// ---------------------------------------------------------------------------
TEST(TransferBrpcTest, TransferManagerRegisterAndConnect) {
    TransferConfig config;
    config.protocol = "brpc";

    TransferManager mgr(config);

    // Register an address that doesn't have a server.
    mgr.RegisterStoreAddr(1, "127.0.0.1:18999");

    // GetStoreChannel will attempt to connect to 127.0.0.1:18999.
    // Since no server is running, CreateChannel will fail and return nullptr.
    // This validates the connection attempt path.
    TransferChannel* ch = mgr.GetStoreChannel(1);
    // Connection will likely fail, so nullptr is expected.
    // If a server happens to be running, it would succeed.
    // We just verify no crash.
    (void)ch;
}

// ---------------------------------------------------------------------------
// MT-T-005: TransferManager CloseAll does not crash
// ---------------------------------------------------------------------------
TEST(TransferBrpcTest, TransferManagerCloseAll) {
    TransferConfig config;
    config.protocol = "brpc";

    TransferManager mgr(config);

    mgr.RegisterStoreAddr(1, "127.0.0.1:18901");
    mgr.RegisterStoreAddr(2, "127.0.0.1:18902");

    // CloseAll should not crash even without active connections.
    mgr.CloseAll();
    // Second CloseAll should also be safe.
    mgr.CloseAll();
}

#include <gtest/gtest.h>
#include <string>

#include "src/transfer/transfer_manager.h"

using namespace falconkv;

// ---------------------------------------------------------------------------
// Constructor with config
// ---------------------------------------------------------------------------
TEST(TransferManager, ConstructorWithConfig) {
    TransferConfig config;
    config.protocol = "brpc";
    config.meta_addr = "localhost:8900";
    config.meta_pool_size = 2;
    config.store_pool_size = 2;
    config.rpc_timeout_ms = 1000;

    TransferManager mgr(config);
    // Construction should succeed without crash.
}

// ---------------------------------------------------------------------------
// RegisterStoreAddr
// ---------------------------------------------------------------------------
TEST(TransferManager, RegisterStoreAddr) {
    TransferConfig config;
    config.meta_addr = "";

    TransferManager mgr(config);

    // Registering an address should not crash.
    mgr.RegisterStoreAddr(1, "10.0.0.1:8901");
    mgr.RegisterStoreAddr(2, "10.0.0.2:8901");

    // Re-registering should also be fine.
    mgr.RegisterStoreAddr(1, "10.0.0.1:8901");
}

// ---------------------------------------------------------------------------
// GetStoreChannel returns nullptr without addr
// ---------------------------------------------------------------------------
TEST(TransferManager, GetStoreChannelNullWithoutAddr) {
    TransferConfig config;
    TransferManager mgr(config);

    // No address registered for store_id 99.
    TransferChannel* ch = mgr.GetStoreChannel(99);
    EXPECT_EQ(ch, nullptr);
}

TEST(TransferManager, GetStoreChannelNullEvenWithAddr) {
    // Without brpc, CreateChannel returns nullptr, so GetStoreChannel
    // should return nullptr even when an address is registered.
    TransferConfig config;
    TransferManager mgr(config);

    mgr.RegisterStoreAddr(1, "localhost:8901");

    TransferChannel* ch = mgr.GetStoreChannel(1);
    // Without brpc, channel creation returns nullptr.
    EXPECT_EQ(ch, nullptr);
}

// ---------------------------------------------------------------------------
// GetMetaChannel returns nullptr without meta_addr
// ---------------------------------------------------------------------------
TEST(TransferManager, GetMetaChannelNullWithoutAddr) {
    TransferConfig config;
    config.meta_addr = "";

    TransferManager mgr(config);
    TransferChannel* ch = mgr.GetMetaChannel();
    EXPECT_EQ(ch, nullptr);
}

TEST(TransferManager, GetMetaChannelNullWithAddrButNoBrpc) {
    TransferConfig config;
    config.meta_addr = "localhost:8900";

    TransferManager mgr(config);
    // Without brpc, CreateChannel returns nullptr.
    TransferChannel* ch = mgr.GetMetaChannel();
    EXPECT_EQ(ch, nullptr);
}

// ---------------------------------------------------------------------------
// CloseAll
// ---------------------------------------------------------------------------
TEST(TransferManager, CloseAllNoopOnEmpty) {
    TransferConfig config;
    TransferManager mgr(config);

    // CloseAll on an empty manager should not crash.
    mgr.CloseAll();
}

TEST(TransferManager, CloseAllAfterRegister) {
    TransferConfig config;
    TransferManager mgr(config);

    mgr.RegisterStoreAddr(1, "10.0.0.1:8901");
    mgr.RegisterStoreAddr(2, "10.0.0.2:8901");

    // CloseAll should clear internal state without crash.
    mgr.CloseAll();

    // After CloseAll, getting channels should still return nullptr.
    EXPECT_EQ(mgr.GetStoreChannel(1), nullptr);
}

// ---------------------------------------------------------------------------
// GetStoreChannels (batch)
// ---------------------------------------------------------------------------
TEST(TransferManager, GetStoreChannelsEmptyInput) {
    TransferConfig config;
    TransferManager mgr(config);

    auto channels = mgr.GetStoreChannels({});
    EXPECT_TRUE(channels.empty());
}

TEST(TransferManager, GetStoreChannelsMultipleIds) {
    TransferConfig config;
    TransferManager mgr(config);

    mgr.RegisterStoreAddr(1, "10.0.0.1:8901");
    mgr.RegisterStoreAddr(2, "10.0.0.2:8901");

    auto channels = mgr.GetStoreChannels({1, 2, 3});
    // Without brpc, all channels should be nullptr, so the result is empty.
    EXPECT_TRUE(channels.empty());
}

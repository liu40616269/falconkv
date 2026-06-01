#include <gtest/gtest.h>

#include "src/store/meta_sync_client.h"

using namespace falconkv;

// ---------------------------------------------------------------------------
// MetaSyncClient: Connect with empty address (skip mode)
// ---------------------------------------------------------------------------
TEST(MetaSyncClientTest, ConnectEmptyAddr) {
    MetaSyncClient client;
    Status s = client.Connect("");
    EXPECT_TRUE(s.ok());
    EXPECT_FALSE(client.connected());
}

// ---------------------------------------------------------------------------
// MetaSyncClient: SyncCommit with no connection (skip mode)
// ---------------------------------------------------------------------------
TEST(MetaSyncClientTest, SyncCommitSkippedWhenNotConnected) {
    MetaSyncClient client;
    // Not connected — operations should silently succeed (skip)
    std::vector<StoreKeyRecord> records;
    StoreKeyRecord rec;
    rec.key = "key1";
    rec.offset = 100;
    rec.size = 50;
    rec.chunk_size = 2048;
    records.push_back(rec);

    Status s = client.SyncCommit(1, records);
    EXPECT_TRUE(s.ok());
}

// ---------------------------------------------------------------------------
// MetaSyncClient: SyncRemove with no connection (skip mode)
// ---------------------------------------------------------------------------
TEST(MetaSyncClientTest, SyncRemoveSkippedWhenNotConnected) {
    MetaSyncClient client;
    std::vector<std::string> keys = {"key1", "key2"};

    Status s = client.SyncRemove(1, keys);
    EXPECT_TRUE(s.ok());
}

// ---------------------------------------------------------------------------
// MetaSyncClient: RegisterStore with no connection (skip mode)
// ---------------------------------------------------------------------------
TEST(MetaSyncClientTest, RegisterStoreSkippedWhenNotConnected) {
    MetaSyncClient client;

    Status s = client.RegisterStore(1, 1, "/data/kv_data_1", 1024 * 1024, 2048);
    EXPECT_TRUE(s.ok());
}

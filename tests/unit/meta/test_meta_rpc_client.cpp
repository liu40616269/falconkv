#include <gtest/gtest.h>

#include <thread>
#include <chrono>

#include "src/meta/meta_manager.h"
#include "src/meta/meta_server.h"
#include "src/meta/meta_rpc_client.h"
#include "src/common/config.h"

namespace falconkv {

class MetaRpcClientTest : public ::testing::Test {
protected:
    void SetUp() override {
        MetaConfig cfg;
        cfg.listen_addr = "127.0.0.1:19876";

        server_ = std::make_unique<MetaServer>(cfg);

        // Register a store
        StoreInfo info;
        info.store_id = 1;
        info.node_id = 0;
        info.store_addr = "localhost:8901";
        info.data_file = "/tmp/falconkv_test_rpc/data_0.db";

        ASSERT_TRUE(server_->GetMetaManager()->RegisterStore(info).ok());
        ASSERT_TRUE(server_->Start().ok());

        // Connect the RPC client.
        ASSERT_TRUE(client_.Connect("127.0.0.1:19876").ok());
    }

    void TearDown() override {
        server_->Stop();
    }

    std::unique_ptr<MetaServer> server_;
    MetaRpcClient client_;
};

TEST_F(MetaRpcClientTest, BatchExistEmpty) {
    // No keys committed yet, all should miss
    auto records = client_.BatchExist({"nonexistent1", "nonexistent2"});
    ASSERT_EQ(records.size(), 2u);
    EXPECT_TRUE(records[0].key.empty());
    EXPECT_TRUE(records[1].key.empty());
}

TEST_F(MetaRpcClientTest, BatchExistAfterSyncCommit) {
    // Use MetaManager directly to SyncCommit keys (simulating Store behavior)
    MetaManager* mgr = server_->GetMetaManager();

    std::vector<KeyRecord> records;
    KeyRecord rec1;
    rec1.key = "exist_key";
    rec1.offset = 100;
    rec1.size = 50;
    records.push_back(rec1);

    mgr->SyncCommit(1, records);

    // Now BatchExist via RPC
    auto results = client_.BatchExist({"exist_key", "nonexistent"});
    ASSERT_EQ(results.size(), 2u);
    EXPECT_EQ(results[0].key, "exist_key");
    EXPECT_EQ(results[0].stat, 1);
    EXPECT_TRUE(results[1].key.empty());
}

TEST_F(MetaRpcClientTest, BatchLookup) {
    // SyncCommit a key
    MetaManager* mgr = server_->GetMetaManager();
    std::vector<KeyRecord> records;
    KeyRecord rec;
    rec.key = "lookup_key";
    rec.offset = 200;
    rec.size = 75;
    records.push_back(rec);
    mgr->SyncCommit(1, records);

    // Lookup via RPC
    auto results = client_.BatchLookup({"lookup_key", "nonexistent"});
    ASSERT_EQ(results.size(), 2u);
    EXPECT_EQ(results[0].key, "lookup_key");
    EXPECT_TRUE(results[1].key.empty());
}

TEST_F(MetaRpcClientTest, BatchExistAfterSyncRemove) {
    // SyncCommit then SyncRemove
    MetaManager* mgr = server_->GetMetaManager();
    std::vector<KeyRecord> records;
    KeyRecord rec;
    rec.key = "remove_key";
    rec.offset = 300;
    rec.size = 100;
    records.push_back(rec);
    mgr->SyncCommit(1, records);

    // Verify it exists
    auto exist1 = client_.BatchExist({"remove_key"});
    EXPECT_EQ(exist1[0].key, "remove_key");

    // Remove
    mgr->SyncRemove(1, {"remove_key"});

    // Verify it no longer exists
    auto exist2 = client_.BatchExist({"remove_key"});
    EXPECT_TRUE(exist2[0].key.empty());
}

} // namespace falconkv

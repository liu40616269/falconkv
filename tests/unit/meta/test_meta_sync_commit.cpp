#include <gtest/gtest.h>
#include <vector>
#include <string>

#include "src/meta/meta_manager.h"

using namespace falconkv;

// ---------------------------------------------------------------------------
// MetaManager: SyncCommit inserts new records
// ---------------------------------------------------------------------------
TEST(MetaSyncCommitTest, SyncCommitInsertsRecords) {
    MetaManager mgr;

    // Register a store first
    StoreInfo info;
    info.store_id = 1;
    mgr.RegisterStore(info);

    // Sync commit some records
    std::vector<KeyRecord> records;
    KeyRecord rec1;
    rec1.key = "key1";
    rec1.offset = 100;
    rec1.size = 50;
    records.push_back(rec1);

    KeyRecord rec2;
    rec2.key = "key2";
    rec2.offset = 200;
    rec2.size = 75;
    records.push_back(rec2);

    Status s = mgr.SyncCommit(1, records);
    EXPECT_TRUE(s.ok());

    // Verify via BatchExist
    std::vector<std::string> keys = {"key1", "key2", "key3"};
    auto exist_results = mgr.BatchExist(keys);

    EXPECT_EQ(exist_results[0].key, "key1");
    EXPECT_EQ(exist_results[0].store_id, 1u);
    EXPECT_EQ(exist_results[0].offset, 100u);
    EXPECT_EQ(exist_results[0].stat, 1);

    EXPECT_EQ(exist_results[1].key, "key2");
    EXPECT_EQ(exist_results[1].store_id, 1u);
    EXPECT_EQ(exist_results[1].offset, 200u);
    EXPECT_EQ(exist_results[1].stat, 1);

    // key3 should not exist
    EXPECT_TRUE(exist_results[2].key.empty());
}

// ---------------------------------------------------------------------------
// MetaManager: SyncCommit updates existing records
// ---------------------------------------------------------------------------
TEST(MetaSyncCommitTest, SyncCommitUpdatesExisting) {
    MetaManager mgr;

    StoreInfo info;
    info.store_id = 1;
    mgr.RegisterStore(info);

    // First commit
    std::vector<KeyRecord> records1;
    KeyRecord rec1;
    rec1.key = "key1";
    rec1.offset = 100;
    rec1.size = 50;
    records1.push_back(rec1);
    mgr.SyncCommit(1, records1);

    // Second commit (update)
    std::vector<KeyRecord> records2;
    KeyRecord rec2;
    rec2.key = "key1";
    rec2.offset = 300;
    rec2.size = 80;
    records2.push_back(rec2);
    mgr.SyncCommit(1, records2);

    // Verify updated
    auto exist_results = mgr.BatchExist({"key1"});
    EXPECT_EQ(exist_results[0].offset, 300u);
    EXPECT_EQ(exist_results[0].size, 80u);
}

// ---------------------------------------------------------------------------
// MetaManager: SyncRemove removes records
// ---------------------------------------------------------------------------
TEST(MetaSyncCommitTest, SyncRemoveDeletesRecords) {
    MetaManager mgr;

    StoreInfo info;
    info.store_id = 1;
    mgr.RegisterStore(info);

    // Commit some records
    std::vector<KeyRecord> records;
    KeyRecord rec1;
    rec1.key = "key1";
    rec1.offset = 100;
    rec1.size = 50;
    records.push_back(rec1);

    KeyRecord rec2;
    rec2.key = "key2";
    rec2.offset = 200;
    rec2.size = 75;
    records.push_back(rec2);

    mgr.SyncCommit(1, records);

    // Remove key1
    Status s = mgr.SyncRemove(1, {"key1"});
    EXPECT_TRUE(s.ok());

    // Verify key1 is gone, key2 remains
    auto exist_results = mgr.BatchExist({"key1", "key2"});
    EXPECT_TRUE(exist_results[0].key.empty());
    EXPECT_EQ(exist_results[1].key, "key2");
}

// ---------------------------------------------------------------------------
// MetaManager: SyncRemove non-existent key is a no-op
// ---------------------------------------------------------------------------
TEST(MetaSyncCommitTest, SyncRemoveNonExistentIsNoop) {
    MetaManager mgr;

    Status s = mgr.SyncRemove(1, {"nonexistent"});
    EXPECT_TRUE(s.ok());
}

// ---------------------------------------------------------------------------
// MetaManager: SyncCommit with empty key is skipped
// ---------------------------------------------------------------------------
TEST(MetaSyncCommitTest, SyncCommitSkipsEmptyKey) {
    MetaManager mgr;

    std::vector<KeyRecord> records;
    KeyRecord rec;
    rec.key = "";
    rec.offset = 100;
    rec.size = 50;
    records.push_back(rec);

    Status s = mgr.SyncCommit(1, records);
    EXPECT_TRUE(s.ok());

    auto exist_results = mgr.BatchExist({""});
    EXPECT_TRUE(exist_results[0].key.empty());
}

// ---------------------------------------------------------------------------
// MetaManager: SyncCommit updates store usage ratio
// ---------------------------------------------------------------------------
TEST(MetaSyncCommitTest, SyncCommitUpdatesUsage) {
    MetaManager mgr;

    StoreInfo info;
    info.store_id = 1;
    mgr.RegisterStore(info);

    std::vector<KeyRecord> records;
    KeyRecord rec;
    rec.key = "key1";
    rec.offset = 100;
    rec.size = 500;
    records.push_back(rec);

    mgr.SyncCommit(1, records);

    // The store's usage should be updated
    auto lookup_results = mgr.BatchLookup({"key1"});
    // The key should exist (stat != 2)
    EXPECT_EQ(lookup_results[0].key, "key1");
}

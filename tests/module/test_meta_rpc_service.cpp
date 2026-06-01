#include <gtest/gtest.h>
#include <string>
#include <vector>

#include "src/meta/meta_manager.h"

using namespace falconkv;

// ---------------------------------------------------------------------------
// MT-M-001: RegisterStore makes keys visible via BatchExist
// ---------------------------------------------------------------------------
TEST(MetaRpcServiceTest, StoreRegister) {
    MetaManager mgr(4);

    StoreInfo info;
    info.store_id = 1;
    info.node_id = 10;
    info.store_addr = "10.0.0.1:8901";
    info.data_file = "/data/kv_data_1";
    info.chunk_size = 2097152;

    Status s = mgr.RegisterStore(info);
    ASSERT_TRUE(s.ok()) << "RegisterStore failed: " << s.msg();

    // SyncCommit a committed record for store 1.
    KeyRecord rec;
    rec.key = "key1";
    rec.store_id = 1;
    rec.offset = 0;
    rec.size = 4096;
    rec.stat = 1;
    rec.node_id = 10;

    s = mgr.SyncCommit(1, {rec});
    ASSERT_TRUE(s.ok());

    // BatchExist should now find "key1".
    auto results = mgr.BatchExist({"key1"});
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].key, "key1");
    EXPECT_EQ(results[0].stat, 1);
}

// ---------------------------------------------------------------------------
// MT-M-002: BatchExist with mixed hit/miss keys
// ---------------------------------------------------------------------------
TEST(MetaRpcServiceTest, BatchExistBatchQuery) {
    MetaManager mgr(4);

    StoreInfo info;
    info.store_id = 1;
    info.node_id = 1;
    info.store_addr = "localhost:8901";
    info.chunk_size = 4096;
    mgr.RegisterStore(info);

    // Commit 5 records.
    std::vector<KeyRecord> records;
    for (int i = 0; i < 5; ++i) {
        KeyRecord rec;
        rec.key = "exist_key" + std::to_string(i);
        rec.store_id = 1;
        rec.offset = i * 4096;
        rec.size = 4096;
        rec.stat = 1;
        records.push_back(rec);
    }
    ASSERT_TRUE(mgr.SyncCommit(1, records).ok());

    // Query with a mix of existing and non-existing keys.
    std::vector<std::string> query_keys;
    for (int i = 0; i < 5; ++i) query_keys.push_back("exist_key" + std::to_string(i));
    for (int i = 0; i < 5; ++i) query_keys.push_back("missing_key" + std::to_string(i));

    auto results = mgr.BatchExist(query_keys);
    // BatchExist returns a vector the same size as input.
    // Hits have non-empty key, misses have empty key.
    ASSERT_EQ(results.size(), 10u);
    int hit_count = 0;
    for (const auto& r : results) {
        if (!r.key.empty()) {
            EXPECT_EQ(r.stat, 1);
            hit_count++;
        }
    }
    EXPECT_EQ(hit_count, 5) << "Expected 5 hits from BatchExist";
}

// ---------------------------------------------------------------------------
// MT-M-003: SyncCommit always sets stat=1 (committed), visible via BatchExist
// ---------------------------------------------------------------------------
TEST(MetaRpcServiceTest, SyncCommitMakesVisible) {
    MetaManager mgr(4);

    StoreInfo info;
    info.store_id = 2;
    info.node_id = 2;
    mgr.RegisterStore(info);

    // SyncCommit a record — note that SyncCommit always sets stat=1
    // regardless of the input record's stat field.
    KeyRecord rec;
    rec.key = "commit_key";
    rec.store_id = 2;
    rec.stat = 0;

    ASSERT_TRUE(mgr.SyncCommit(2, {rec}).ok());

    // BatchExist should find the key (SyncCommit always commits with stat=1).
    auto results = mgr.BatchExist({"commit_key"});
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].key, "commit_key");
    EXPECT_EQ(results[0].stat, 1);
}

// ---------------------------------------------------------------------------
// MT-M-004: SyncRemove removes keys from BatchExist
// ---------------------------------------------------------------------------
TEST(MetaRpcServiceTest, SyncRemoveRemovesKeys) {
    MetaManager mgr(4);

    StoreInfo info;
    info.store_id = 3;
    info.node_id = 3;
    mgr.RegisterStore(info);

    KeyRecord rec;
    rec.key = "to_remove";
    rec.store_id = 3;
    rec.stat = 1;
    ASSERT_TRUE(mgr.SyncCommit(3, {rec}).ok());

    // Verify it exists (non-empty key in result).
    auto results = mgr.BatchExist({"to_remove"});
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].key, "to_remove");

    // Remove it.
    ASSERT_TRUE(mgr.SyncRemove(3, {"to_remove"}).ok());

    // Should no longer be found (empty key in result).
    results = mgr.BatchExist({"to_remove"});
    ASSERT_EQ(results.size(), 1u);
    EXPECT_TRUE(results[0].key.empty()) << "Key should be removed after SyncRemove";
}

// ---------------------------------------------------------------------------
// MT-M-005: BatchLookup returns records regardless of stat (but not evicted)
// ---------------------------------------------------------------------------
TEST(MetaRpcServiceTest, BatchLookupReturnsAllNonEvicted) {
    MetaManager mgr(4);

    StoreInfo info;
    info.store_id = 5;
    info.node_id = 5;
    mgr.RegisterStore(info);

    // Commit two keys.
    std::vector<KeyRecord> records;
    for (int i = 0; i < 2; ++i) {
        KeyRecord rec;
        rec.key = "lookup_key" + std::to_string(i);
        rec.store_id = 5;
        rec.stat = 1;
        records.push_back(rec);
    }
    ASSERT_TRUE(mgr.SyncCommit(5, records).ok());

    // BatchLookup should find both keys.
    auto results = mgr.BatchLookup({"lookup_key0", "lookup_key1", "nonexistent"});
    ASSERT_EQ(results.size(), 3u);

    int found = 0;
    for (const auto& r : results) {
        if (!r.key.empty()) found++;
    }
    EXPECT_EQ(found, 2) << "BatchLookup should find 2 committed keys";
}

#include <gtest/gtest.h>
#include <vector>
#include <string>

#include "src/store/store_meta_index.h"

using namespace falconkv;

// ---------------------------------------------------------------------------
// StoreMetaIndex: Put + Get
// ---------------------------------------------------------------------------
TEST(StoreMetaIndexTest, PutAndGet) {
    StoreMetaIndex idx;

    StoreKeyRecord rec;
    rec.key = "key1";
    rec.offset = 1024;
    rec.size = 512;
    rec.alloc_size = 2048;
    rec.stat = 1;
    rec.access_time_ms = 1000;

    idx.Put("key1", rec);

    auto result = idx.Get("key1");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->key, "key1");
    EXPECT_EQ(result->offset, 1024u);
    EXPECT_EQ(result->size, 512u);
    EXPECT_EQ(result->stat, 1);
}

TEST(StoreMetaIndexTest, GetMissingKey) {
    StoreMetaIndex idx;
    auto result = idx.Get("nonexistent");
    EXPECT_FALSE(result.has_value());
}

TEST(StoreMetaIndexTest, GetUncommittedKey) {
    StoreMetaIndex idx;
    StoreKeyRecord rec;
    rec.key = "key1";
    rec.stat = 0; // allocated but not committed
    idx.Put("key1", rec);

    auto result = idx.Get("key1");
    EXPECT_FALSE(result.has_value());
}

// ---------------------------------------------------------------------------
// StoreMetaIndex: BatchContains
// ---------------------------------------------------------------------------
TEST(StoreMetaIndexTest, BatchContainsMixed) {
    StoreMetaIndex idx;

    // Insert committed keys
    StoreKeyRecord rec1;
    rec1.key = "key1";
    rec1.offset = 100;
    rec1.size = 10;
    rec1.stat = 1;
    idx.Put("key1", rec1);

    StoreKeyRecord rec2;
    rec2.key = "key2";
    rec2.offset = 200;
    rec2.size = 20;
    rec2.stat = 1;
    idx.Put("key2", rec2);

    // Insert uncommitted key
    StoreKeyRecord rec3;
    rec3.key = "key3";
    rec3.stat = 0;
    idx.Put("key3", rec3);

    std::vector<std::string> keys = {"key1", "key3", "key4", "key2"};
    std::vector<StoreKeyRecord> hits;
    std::vector<std::string> misses;

    idx.BatchContains(keys, hits, misses);

    EXPECT_EQ(hits.size(), 2u);
    EXPECT_EQ(misses.size(), 2u);

    // Misses should be key3 (uncommitted) and key4 (nonexistent)
    EXPECT_EQ(misses[0], "key3");
    EXPECT_EQ(misses[1], "key4");
}

// ---------------------------------------------------------------------------
// StoreMetaIndex: Commit
// ---------------------------------------------------------------------------
TEST(StoreMetaIndexTest, CommitTransitions) {
    StoreMetaIndex idx;

    StoreKeyRecord rec;
    rec.key = "key1";
    rec.stat = 0;
    idx.Put("key1", rec);

    // Before commit, Get returns nullopt (stat != 1)
    EXPECT_FALSE(idx.Get("key1").has_value());

    // Commit
    EXPECT_TRUE(idx.Commit("key1"));

    // After commit, Get returns the record
    auto result = idx.Get("key1");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->stat, 1);

    // Commit non-existent key
    EXPECT_FALSE(idx.Commit("nonexistent"));
}

// ---------------------------------------------------------------------------
// StoreMetaIndex: Remove
// ---------------------------------------------------------------------------
TEST(StoreMetaIndexTest, RemoveExisting) {
    StoreMetaIndex idx;

    StoreKeyRecord rec;
    rec.key = "key1";
    rec.offset = 100;
    rec.stat = 1;
    idx.Put("key1", rec);

    auto removed = idx.Remove("key1");
    ASSERT_TRUE(removed.has_value());
    EXPECT_EQ(removed->key, "key1");
    EXPECT_EQ(removed->offset, 100u);

    // After removal, Get returns nullopt
    EXPECT_FALSE(idx.Get("key1").has_value());
}

TEST(StoreMetaIndexTest, RemoveNonexistent) {
    StoreMetaIndex idx;
    auto result = idx.Remove("nonexistent");
    EXPECT_FALSE(result.has_value());
}

// ---------------------------------------------------------------------------
// StoreMetaIndex: Touch updates access time
// ---------------------------------------------------------------------------
TEST(StoreMetaIndexTest, TouchUpdatesAccessTime) {
    StoreMetaIndex idx;

    StoreKeyRecord rec;
    rec.key = "key1";
    rec.stat = 1;
    rec.access_time_ms = 1000;
    idx.Put("key1", rec);

    idx.Touch("key1");

    auto result = idx.Get("key1");
    ASSERT_TRUE(result.has_value());
    EXPECT_GT(result->access_time_ms, 1000u);
}

// ---------------------------------------------------------------------------
// StoreMetaIndex: CommittedCount
// ---------------------------------------------------------------------------
TEST(StoreMetaIndexTest, CommittedCount) {
    StoreMetaIndex idx;

    EXPECT_EQ(idx.CommittedCount(), 0u);

    StoreKeyRecord rec1;
    rec1.key = "key1";
    rec1.stat = 1;
    idx.Put("key1", rec1);

    StoreKeyRecord rec2;
    rec2.key = "key2";
    rec2.stat = 1;
    idx.Put("key2", rec2);

    StoreKeyRecord rec3;
    rec3.key = "key3";
    rec3.stat = 0;
    idx.Put("key3", rec3);

    EXPECT_EQ(idx.CommittedCount(), 2u);

    idx.Remove("key1");
    EXPECT_EQ(idx.CommittedCount(), 1u);
}

// ---------------------------------------------------------------------------
// StoreMetaIndex: Put overwrites existing
// ---------------------------------------------------------------------------
TEST(StoreMetaIndexTest, PutOverwrites) {
    StoreMetaIndex idx;

    StoreKeyRecord rec1;
    rec1.key = "key1";
    rec1.offset = 100;
    rec1.stat = 1;
    idx.Put("key1", rec1);

    StoreKeyRecord rec2;
    rec2.key = "key1";
    rec2.offset = 200;
    rec2.stat = 1;
    idx.Put("key1", rec2);

    auto result = idx.Get("key1");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->offset, 200u);
}

// ---------------------------------------------------------------------------
// StoreMetaIndex: GetColdEntries
// ---------------------------------------------------------------------------
TEST(StoreMetaIndexTest, GetColdEntriesBasic) {
    StoreMetaIndex idx;

    // Insert committed keys with different access times
    StoreKeyRecord rec1;
    rec1.key = "cold1";
    rec1.offset = 100;
    rec1.stat = 1;
    rec1.access_time_ms = 1000;
    idx.Put("cold1", rec1);

    StoreKeyRecord rec2;
    rec2.key = "cold2";
    rec2.offset = 200;
    rec2.stat = 1;
    rec2.access_time_ms = 2000;
    idx.Put("cold2", rec2);

    StoreKeyRecord rec3;
    rec3.key = "hot";
    rec3.offset = 300;
    rec3.stat = 1;
    rec3.access_time_ms = 10000;
    idx.Put("hot", rec3);

    // Insert uncommitted key — should be excluded
    StoreKeyRecord rec4;
    rec4.key = "uncommitted";
    rec4.offset = 400;
    rec4.stat = 0;
    rec4.access_time_ms = 500;
    idx.Put("uncommitted", rec4);

    // threshold=5000: cold1 and cold2 qualify, hot does not
    auto cold = idx.GetColdEntries(5000, 10);
    ASSERT_EQ(cold.size(), 2u);
    // Sorted by access_time ascending
    EXPECT_EQ(cold[0].key, "cold1");
    EXPECT_EQ(cold[1].key, "cold2");
}

TEST(StoreMetaIndexTest, GetColdEntriesMaxCount) {
    StoreMetaIndex idx;

    for (int i = 0; i < 10; ++i) {
        StoreKeyRecord rec;
        rec.key = "key" + std::to_string(i);
        rec.offset = static_cast<uint64_t>(i * 100);
        rec.stat = 1;
        rec.access_time_ms = static_cast<uint64_t>(i * 100);
        idx.Put(rec.key, rec);
    }

    // Request at most 3
    auto cold = idx.GetColdEntries(10000, 3);
    ASSERT_EQ(cold.size(), 3u);
    EXPECT_EQ(cold[0].key, "key0");
    EXPECT_EQ(cold[1].key, "key1");
    EXPECT_EQ(cold[2].key, "key2");
}

TEST(StoreMetaIndexTest, GetColdEntriesEmpty) {
    StoreMetaIndex idx;
    auto cold = idx.GetColdEntries(5000, 10);
    EXPECT_TRUE(cold.empty());
}

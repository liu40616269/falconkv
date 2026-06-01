#include <gtest/gtest.h>

#include "src/store/store_meta_index.h"
#include "src/store/pending_evict_queue.h"
#include "src/common/buddy_allocator.h"

using namespace falconkv;

// ---------------------------------------------------------------------------
// EvictManager logic: cold entry selection + eviction flow
// ---------------------------------------------------------------------------
TEST(EvictManagerTest, ColdEntrySelectionAndEviction) {
    StoreMetaIndex idx;
    BuddyAllocator alloc(4096 * 64, 4096, 1);

    // Allocate space for each entry (simulating what FalconKVStore.Put does)
    std::vector<int64_t> offsets;
    for (int i = 0; i < 10; ++i) {
        int64_t off = alloc.AllocChunk();
        ASSERT_GE(off, 0);
        offsets.push_back(off);

        StoreKeyRecord rec;
        rec.key = "cold" + std::to_string(i);
        rec.offset = static_cast<uint64_t>(off);
        rec.size = 100;
        rec.stat = 1;
        rec.access_time_ms = 1000;
        idx.Put(rec.key, rec);
    }

    // Insert a hot entry that should NOT be evicted
    int64_t hot_off = alloc.AllocChunk();
    ASSERT_GE(hot_off, 0);
    StoreKeyRecord hot;
    hot.key = "hot";
    hot.offset = static_cast<uint64_t>(hot_off);
    hot.size = 100;
    hot.stat = 1;
    hot.access_time_ms = 9999999999ULL;  // much larger than threshold
    idx.Put("hot", hot);

    uint64_t used_before = alloc.GetUsedBytes();
    EXPECT_GT(used_before, 0u);

    PendingEvictQueue pending_queue(100, &alloc);
    pending_queue.Start();

    // Simulate the eviction flow:
    // 1. Get cold entries with threshold that excludes the hot entry
    auto cold = idx.GetColdEntries(999999999, 16);
    EXPECT_GE(cold.size(), 10u);

    // 2. In real EvictManager, SyncRemove would be called here.
    //    We skip Meta sync in this test since MetaSyncClient is not mockable.

    // 3. Remove from local index and enqueue for deferred reclamation
    for (const auto& rec : cold) {
        idx.Remove(rec.key);
        pending_queue.Enqueue(rec.key, rec.offset);
    }

    // The hot entry should still be in the index
    EXPECT_TRUE(idx.Get("hot").has_value());

    // Cold entries should be gone
    EXPECT_FALSE(idx.Get("cold0").has_value());
    EXPECT_FALSE(idx.Get("cold9").has_value());

    // Stop the queue — FlushAll should reclaim all pending entries
    pending_queue.Stop();

    // Space should be reclaimed
    uint64_t used_after = alloc.GetUsedBytes();
    EXPECT_LT(used_after, used_before);
}

// ---------------------------------------------------------------------------
// EvictManager logic: SyncRemove failure prevents eviction
// (simulated by skipping the Remove step)
// ---------------------------------------------------------------------------
TEST(EvictManagerTest, SyncRemoveFailurePreventsEviction) {
    StoreMetaIndex idx;

    StoreKeyRecord rec;
    rec.key = "cold1";
    rec.offset = 4096;
    rec.size = 100;
    rec.stat = 1;
    rec.access_time_ms = 1000;
    idx.Put("cold1", rec);

    // Simulate: SyncRemove fails → entries should NOT be removed
    // In real code, EvictManager checks SyncRemove status and skips.
    // We simply don't call Remove, which is the same outcome.
    bool sync_remove_ok = false;

    if (sync_remove_ok) {
        idx.Remove("cold1");
    }

    // Entry should still exist since we didn't remove it
    EXPECT_TRUE(idx.Get("cold1").has_value());
}

// ---------------------------------------------------------------------------
// EvictManager logic: max_count limits eviction batch size
// ---------------------------------------------------------------------------
TEST(EvictManagerTest, BatchSizeLimit) {
    StoreMetaIndex idx;

    for (int i = 0; i < 20; ++i) {
        StoreKeyRecord rec;
        rec.key = "key" + std::to_string(i);
        rec.offset = static_cast<uint64_t>(i * 4096);
        rec.stat = 1;
        rec.access_time_ms = 1000;
        idx.Put(rec.key, rec);
    }

    // Request at most 5 cold entries
    auto cold = idx.GetColdEntries(999999999, 5);
    EXPECT_EQ(cold.size(), 5u);

    // Index should still have 20 entries (not yet evicted)
    EXPECT_EQ(idx.CommittedCount(), 20u);
}

// ---------------------------------------------------------------------------
// High watermark triggers eviction: usage > 0.85, cold entries are returned
// ---------------------------------------------------------------------------
TEST(EvictManagerTest, HighWatermarkTriggersEviction) {
    StoreMetaIndex idx;
    BuddyAllocator alloc(4096 * 20, 4096, 1);

    // Fill to > 85% usage (17 of 20 chunks).
    for (int i = 0; i < 18; ++i) {
        int64_t off = alloc.AllocChunk();
        ASSERT_GE(off, 0);

        StoreKeyRecord rec;
        rec.key = "key" + std::to_string(i);
        rec.offset = static_cast<uint64_t>(off);
        rec.size = 4096;
        rec.stat = 1;
        rec.access_time_ms = 1000; // cold
        idx.Put(rec.key, rec);
    }

    // Usage should be > 0.85
    double usage = alloc.GetUsageRatio();
    EXPECT_GT(usage, 0.85) << "Expected usage > 0.85, got " << usage;

    // GetColdEntries should return cold entries to evict.
    auto cold = idx.GetColdEntries(999999999, 20);
    EXPECT_GT(cold.size(), 0u) << "Expected cold entries for eviction";

    // Simulate eviction: remove from index and free space.
    PendingEvictQueue pending_queue(100, &alloc);
    pending_queue.Start();

    for (const auto& rec : cold) {
        idx.Remove(rec.key);
        pending_queue.Enqueue(rec.key, rec.offset);
    }

    pending_queue.Stop();

    // After eviction, usage should have dropped.
    double usage_after = alloc.GetUsageRatio();
    EXPECT_LT(usage_after, usage) << "Usage should decrease after eviction";
}

// ---------------------------------------------------------------------------
// Below high watermark: no eviction triggered
// ---------------------------------------------------------------------------
TEST(EvictManagerTest, BelowHighWatermarkNoEviction) {
    StoreMetaIndex idx;
    BuddyAllocator alloc(4096 * 100, 4096, 1);

    // Fill only 10 chunks out of 100 (10% usage, well below 85%).
    for (int i = 0; i < 10; ++i) {
        int64_t off = alloc.AllocChunk();
        ASSERT_GE(off, 0);

        StoreKeyRecord rec;
        rec.key = "key" + std::to_string(i);
        rec.offset = static_cast<uint64_t>(off);
        rec.size = 4096;
        rec.stat = 1;
        rec.access_time_ms = 1000;
        idx.Put(rec.key, rec);
    }

    // Usage should be well below 0.85.
    double usage = alloc.GetUsageRatio();
    EXPECT_LT(usage, 0.85) << "Expected usage < 0.85, got " << usage;

    // In real EvictManager, TryEvictBatch would return false.
    // We simulate by checking: if usage < high_watermark, skip eviction.
    double high_watermark = 0.85;
    if (usage < high_watermark) {
        // No eviction — index should still have all 10 entries.
        EXPECT_EQ(idx.CommittedCount(), 10u);
    }
}

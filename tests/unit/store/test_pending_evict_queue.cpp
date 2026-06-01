#include <gtest/gtest.h>
#include <thread>
#include <chrono>

#include "src/store/pending_evict_queue.h"
#include "src/common/buddy_allocator.h"

using namespace falconkv;

// ---------------------------------------------------------------------------
// PendingEvictQueue: Enqueue + grace period expiry + space reclamation
// ---------------------------------------------------------------------------
TEST(PendingEvictQueueTest, EnqueueAndGracePeriod) {
    BuddyAllocator alloc(4096 * 64, 4096, 1);

    // Allocate two chunks
    int64_t off1 = alloc.AllocChunk();
    int64_t off2 = alloc.AllocChunk();
    ASSERT_GE(off1, 0);
    ASSERT_GE(off2, 0);

    uint64_t used_after_alloc = alloc.GetUsedBytes();
    EXPECT_GT(used_after_alloc, 0u);

    // Short grace period for testing: 100ms
    PendingEvictQueue queue(100, &alloc);
    queue.Start();

    queue.Enqueue("key1", static_cast<uint64_t>(off1));
    queue.Enqueue("key2", static_cast<uint64_t>(off2));

    EXPECT_EQ(queue.Size(), 2u);

    // Wait for grace period to expire + scan interval
    std::this_thread::sleep_for(std::chrono::milliseconds(400));

    // Entries should have been reclaimed
    EXPECT_EQ(queue.Size(), 0u);

    queue.Stop();

    // Space should be freed
    uint64_t used_after_free = alloc.GetUsedBytes();
    EXPECT_LT(used_after_free, used_after_alloc);
}

// ---------------------------------------------------------------------------
// PendingEvictQueue: Stop flushes immediately (ignores grace period)
// ---------------------------------------------------------------------------
TEST(PendingEvictQueueTest, StopFlushesImmediately) {
    BuddyAllocator alloc(4096 * 64, 4096, 1);

    int64_t off1 = alloc.AllocChunk();
    int64_t off2 = alloc.AllocChunk();
    ASSERT_GE(off1, 0);
    ASSERT_GE(off2, 0);

    uint64_t used_after_alloc = alloc.GetUsedBytes();

    // Long grace period — entries should NOT expire naturally
    PendingEvictQueue queue(60000, &alloc);
    queue.Start();

    queue.Enqueue("key1", static_cast<uint64_t>(off1));
    queue.Enqueue("key2", static_cast<uint64_t>(off2));

    // Stop immediately without waiting for grace period
    queue.Stop();

    // FlushAll should have reclaimed everything
    EXPECT_EQ(queue.Size(), 0u);

    uint64_t used_after_free = alloc.GetUsedBytes();
    EXPECT_LT(used_after_free, used_after_alloc);
}

// ---------------------------------------------------------------------------
// PendingEvictQueue: Entries within grace period are not freed
// ---------------------------------------------------------------------------
TEST(PendingEvictQueueTest, WithinGracePeriodNotFreed) {
    BuddyAllocator alloc(4096 * 64, 4096, 1);

    int64_t off1 = alloc.AllocChunk();
    ASSERT_GE(off1, 0);

    uint64_t used_after_alloc = alloc.GetUsedBytes();

    // 5-second grace period
    PendingEvictQueue queue(5000, &alloc);
    queue.Start();

    queue.Enqueue("key1", static_cast<uint64_t>(off1));

    // Wait only 200ms — well within 5s grace period
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Entry should still be in the queue
    EXPECT_EQ(queue.Size(), 1u);

    // Space should NOT have been reclaimed yet
    EXPECT_EQ(alloc.GetUsedBytes(), used_after_alloc);

    // Now Stop to clean up
    queue.Stop();

    // After Stop (FlushAll), space should be reclaimed
    EXPECT_LT(alloc.GetUsedBytes(), used_after_alloc);
}

// ---------------------------------------------------------------------------
// PendingEvictQueue: Empty queue Stop is safe
// ---------------------------------------------------------------------------
TEST(PendingEvictQueueTest, EmptyQueueStopIsSafe) {
    BuddyAllocator alloc(4096 * 64, 4096, 1);
    PendingEvictQueue queue(100, &alloc);

    // Start and stop without enqueuing anything
    queue.Start();
    queue.Stop();

    EXPECT_EQ(queue.Size(), 0u);
}

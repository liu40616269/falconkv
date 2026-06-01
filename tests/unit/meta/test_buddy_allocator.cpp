#include <gtest/gtest.h>
#include <vector>
#include <unordered_set>

#include "src/common/buddy_allocator.h"

using namespace falconkv;

// ---------------------------------------------------------------------------
// AllocChunk returns valid offset
// ---------------------------------------------------------------------------
TEST(BuddyAllocator, AllocChunkReturnsValidOffset) {
    // 1 MB total, 4KB page size, 2 pages per chunk = 8KB chunk
    BuddyAllocator alloc(1024 * 1024, 4096, 2);

    int64_t offset = alloc.AllocChunk();
    EXPECT_GE(offset, 0);
    EXPECT_EQ(offset % 4096, 0); // page aligned
}

TEST(BuddyAllocator, AllocChunkReturnsPageAlignedOffset) {
    BuddyAllocator alloc(1024 * 1024, 4096, 4);

    for (int i = 0; i < 10; ++i) {
        int64_t offset = alloc.AllocChunk();
        ASSERT_GE(offset, 0);
        EXPECT_EQ(offset % 4096, 0) << "Iteration " << i;
    }
}

// ---------------------------------------------------------------------------
// AllocChunk until exhaustion
// ---------------------------------------------------------------------------
TEST(BuddyAllocator, AllocUntilExhaustion) {
    // 64 KB total, 4KB page size, 1 page per chunk = 4KB chunk
    // -> 16 chunks total
    uint64_t total = 64 * 1024;
    BuddyAllocator alloc(total, 4096, 1);

    std::vector<int64_t> offsets;
    for (int i = 0; i < 16; ++i) {
        int64_t off = alloc.AllocChunk();
        ASSERT_GE(off, 0) << "Allocation " << i << " should succeed";
        offsets.push_back(off);
    }

    // The next allocation should fail.
    int64_t failed = alloc.AllocChunk();
    EXPECT_LT(failed, 0);

    // All offsets should be unique.
    std::unordered_set<int64_t> unique(offsets.begin(), offsets.end());
    EXPECT_EQ(unique.size(), offsets.size());
}

// ---------------------------------------------------------------------------
// FreeChunk with buddy merge
// ---------------------------------------------------------------------------
TEST(BuddyAllocator, FreeChunkAllowsRealloc) {
    // 64 KB, 4KB page, 1 page per chunk = 16 chunks
    BuddyAllocator alloc(64 * 1024, 4096, 1);

    // Allocate all chunks.
    std::vector<int64_t> offsets;
    for (int i = 0; i < 16; ++i) {
        offsets.push_back(alloc.AllocChunk());
    }

    // Exhausted.
    EXPECT_LT(alloc.AllocChunk(), 0);

    // Free half of them.
    for (int i = 0; i < 8; ++i) {
        alloc.FreeChunk(offsets[i]);
    }

    // Should be able to allocate 8 more.
    for (int i = 0; i < 8; ++i) {
        int64_t off = alloc.AllocChunk();
        EXPECT_GE(off, 0) << "Re-allocation " << i << " should succeed";
    }
}

TEST(BuddyAllocator, FreeChunkBuddyMerge) {
    // Allocate two buddy chunks, free them, then re-allocate.
    // With a small allocator, verify merge allows a larger block.
    BuddyAllocator alloc(64 * 1024, 4096, 1);

    int64_t off0 = alloc.AllocChunk();
    int64_t off1 = alloc.AllocChunk();
    ASSERT_GE(off0, 0);
    ASSERT_GE(off1, 0);

    // Free both -- they should be merged back.
    alloc.FreeChunk(off0);
    alloc.FreeChunk(off1);

    // Usage should be zero now.
    EXPECT_DOUBLE_EQ(alloc.GetUsageRatio(), 0.0);
}

// ---------------------------------------------------------------------------
// GetUsageRatio
// ---------------------------------------------------------------------------
TEST(BuddyAllocator, GetUsageRatioBasic) {
    BuddyAllocator alloc(64 * 1024, 4096, 1);

    EXPECT_DOUBLE_EQ(alloc.GetUsageRatio(), 0.0);

    alloc.AllocChunk();
    // 1 page out of 16 = 1/16
    double expected = 1.0 / 16.0;
    EXPECT_NEAR(alloc.GetUsageRatio(), expected, 0.001);

    // Allocate all remaining.
    for (int i = 0; i < 15; ++i) {
        alloc.AllocChunk();
    }
    EXPECT_DOUBLE_EQ(alloc.GetUsageRatio(), 1.0);
}

// ---------------------------------------------------------------------------
// Repeated alloc/free no leak
// ---------------------------------------------------------------------------
TEST(BuddyAllocator, RepeatedAllocFreeNoLeak) {
    BuddyAllocator alloc(64 * 1024, 4096, 1);

    const int N = 100;
    for (int iter = 0; iter < N; ++iter) {
        int64_t off = alloc.AllocChunk();
        ASSERT_GE(off, 0);
        alloc.FreeChunk(off);
    }

    EXPECT_DOUBLE_EQ(alloc.GetUsageRatio(), 0.0);
    EXPECT_EQ(alloc.GetUsedBytes(), 0u);
}

TEST(BuddyAllocator, AllocNFreeAllUsageZero) {
    BuddyAllocator alloc(64 * 1024, 4096, 1);

    std::vector<int64_t> offsets;
    for (int i = 0; i < 16; ++i) {
        offsets.push_back(alloc.AllocChunk());
    }

    EXPECT_DOUBLE_EQ(alloc.GetUsageRatio(), 1.0);

    for (auto off : offsets) {
        alloc.FreeChunk(off);
    }

    EXPECT_DOUBLE_EQ(alloc.GetUsageRatio(), 0.0);
    EXPECT_EQ(alloc.GetUsedBytes(), 0u);
}

// ---------------------------------------------------------------------------
// GetTotalBytes / GetUsedBytes
// ---------------------------------------------------------------------------
TEST(BuddyAllocator, GetTotalBytes) {
    BuddyAllocator alloc(1024 * 1024, 4096, 2);
    EXPECT_EQ(alloc.GetTotalBytes(), 1024u * 1024u);
}

TEST(BuddyAllocator, GetUsedBytes) {
    BuddyAllocator alloc(64 * 1024, 4096, 1);
    EXPECT_EQ(alloc.GetUsedBytes(), 0u);

    alloc.AllocChunk();
    EXPECT_EQ(alloc.GetUsedBytes(), 4096u);
}

// ---------------------------------------------------------------------------
// Edge cases
// ---------------------------------------------------------------------------
TEST(BuddyAllocator, FreeNegativeOffsetNoop) {
    BuddyAllocator alloc(64 * 1024, 4096, 1);
    // Should not crash.
    alloc.FreeChunk(-1);
    EXPECT_DOUBLE_EQ(alloc.GetUsageRatio(), 0.0);
}

TEST(BuddyAllocator, GetChunkPagesAndPageSize) {
    BuddyAllocator alloc(64 * 1024, 4096, 4);
    EXPECT_EQ(alloc.GetChunkPages(), 4u);
    EXPECT_EQ(alloc.GetPageSize(), 4096u);
}

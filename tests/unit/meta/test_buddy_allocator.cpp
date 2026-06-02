#include <gtest/gtest.h>
#include <vector>
#include <unordered_set>

#include "src/common/buddy_allocator.h"

using namespace falconkv;

// ---------------------------------------------------------------------------
// Alloc returns valid offset
// ---------------------------------------------------------------------------
TEST(BuddyAllocator, AllocReturnsValidOffset) {
    // 1 MB total, 4KB page size
    BuddyAllocator alloc(1024 * 1024, 4096);

    uint32_t alloc_size = 0;
    int64_t offset = alloc.Alloc(4096, &alloc_size);
    EXPECT_GE(offset, 0);
    EXPECT_EQ(offset % 4096, 0); // page aligned
    EXPECT_EQ(alloc_size, 4096u);
}

TEST(BuddyAllocator, AllocReturnsPageAlignedOffset) {
    BuddyAllocator alloc(1024 * 1024, 4096);

    for (int i = 0; i < 10; ++i) {
        uint32_t alloc_size = 0;
        int64_t offset = alloc.Alloc(4096, &alloc_size);
        ASSERT_GE(offset, 0) << "Iteration " << i;
        EXPECT_EQ(offset % 4096, 0) << "Iteration " << i;
    }
}

// ---------------------------------------------------------------------------
// Alloc until exhaustion
// ---------------------------------------------------------------------------
TEST(BuddyAllocator, AllocUntilExhaustion) {
    // 64 KB total, 4KB page size -> 16 pages total
    // Allocating 4KB each -> 16 allocations should succeed
    uint64_t total = 64 * 1024;
    BuddyAllocator alloc(total, 4096);

    std::vector<int64_t> offsets;
    std::vector<uint32_t> alloc_sizes;
    for (int i = 0; i < 16; ++i) {
        uint32_t as = 0;
        int64_t off = alloc.Alloc(4096, &as);
        ASSERT_GE(off, 0) << "Allocation " << i << " should succeed";
        EXPECT_EQ(as, 4096u);
        offsets.push_back(off);
        alloc_sizes.push_back(as);
    }

    // The next allocation should fail.
    int64_t failed = alloc.Alloc(4096);
    EXPECT_LT(failed, 0);

    // All offsets should be unique.
    std::unordered_set<int64_t> unique(offsets.begin(), offsets.end());
    EXPECT_EQ(unique.size(), offsets.size());
}

// ---------------------------------------------------------------------------
// Free with buddy merge
// ---------------------------------------------------------------------------
TEST(BuddyAllocator, FreeAllowsRealloc) {
    // 64 KB, 4KB page = 16 pages
    BuddyAllocator alloc(64 * 1024, 4096);

    // Allocate all pages.
    std::vector<int64_t> offsets;
    std::vector<uint32_t> alloc_sizes;
    for (int i = 0; i < 16; ++i) {
        uint32_t as = 0;
        offsets.push_back(alloc.Alloc(4096, &as));
        alloc_sizes.push_back(as);
    }

    // Exhausted.
    EXPECT_LT(alloc.Alloc(4096), 0);

    // Free half of them.
    for (int i = 0; i < 8; ++i) {
        alloc.Free(offsets[i], alloc_sizes[i]);
    }

    // Should be able to allocate 8 more.
    for (int i = 0; i < 8; ++i) {
        int64_t off = alloc.Alloc(4096);
        EXPECT_GE(off, 0) << "Re-allocation " << i << " should succeed";
    }
}

TEST(BuddyAllocator, FreeBuddyMerge) {
    // Allocate two buddy chunks, free them, then re-allocate.
    // With a small allocator, verify merge allows a larger block.
    BuddyAllocator alloc(64 * 1024, 4096);

    uint32_t as0 = 0, as1 = 0;
    int64_t off0 = alloc.Alloc(4096, &as0);
    int64_t off1 = alloc.Alloc(4096, &as1);
    ASSERT_GE(off0, 0);
    ASSERT_GE(off1, 0);

    // Free both -- they should be merged back.
    alloc.Free(off0, as0);
    alloc.Free(off1, as1);

    // Usage should be zero now.
    EXPECT_DOUBLE_EQ(alloc.GetUsageRatio(), 0.0);
}

// ---------------------------------------------------------------------------
// GetUsageRatio
// ---------------------------------------------------------------------------
TEST(BuddyAllocator, GetUsageRatioBasic) {
    BuddyAllocator alloc(64 * 1024, 4096);

    EXPECT_DOUBLE_EQ(alloc.GetUsageRatio(), 0.0);

    uint32_t as = 0;
    alloc.Alloc(4096, &as);
    // 1 page out of 16 = 1/16
    double expected = 1.0 / 16.0;
    EXPECT_NEAR(alloc.GetUsageRatio(), expected, 0.001);

    // Allocate all remaining.
    for (int i = 0; i < 15; ++i) {
        alloc.Alloc(4096);
    }
    EXPECT_DOUBLE_EQ(alloc.GetUsageRatio(), 1.0);
}

// ---------------------------------------------------------------------------
// Repeated alloc/free no leak
// ---------------------------------------------------------------------------
TEST(BuddyAllocator, RepeatedAllocFreeNoLeak) {
    BuddyAllocator alloc(64 * 1024, 4096);

    const int N = 100;
    for (int iter = 0; iter < N; ++iter) {
        uint32_t as = 0;
        int64_t off = alloc.Alloc(4096, &as);
        ASSERT_GE(off, 0);
        alloc.Free(off, as);
    }

    EXPECT_DOUBLE_EQ(alloc.GetUsageRatio(), 0.0);
    EXPECT_EQ(alloc.GetUsedBytes(), 0u);
}

TEST(BuddyAllocator, AllocNFreeAllUsageZero) {
    BuddyAllocator alloc(64 * 1024, 4096);

    std::vector<int64_t> offsets;
    std::vector<uint32_t> alloc_sizes;
    for (int i = 0; i < 16; ++i) {
        uint32_t as = 0;
        offsets.push_back(alloc.Alloc(4096, &as));
        alloc_sizes.push_back(as);
    }

    EXPECT_DOUBLE_EQ(alloc.GetUsageRatio(), 1.0);

    for (size_t i = 0; i < offsets.size(); ++i) {
        alloc.Free(offsets[i], alloc_sizes[i]);
    }

    EXPECT_DOUBLE_EQ(alloc.GetUsageRatio(), 0.0);
    EXPECT_EQ(alloc.GetUsedBytes(), 0u);
}

// ---------------------------------------------------------------------------
// GetTotalBytes / GetUsedBytes
// ---------------------------------------------------------------------------
TEST(BuddyAllocator, GetTotalBytes) {
    BuddyAllocator alloc(1024 * 1024, 4096);
    EXPECT_EQ(alloc.GetTotalBytes(), 1024u * 1024u);
}

TEST(BuddyAllocator, GetUsedBytes) {
    BuddyAllocator alloc(64 * 1024, 4096);
    EXPECT_EQ(alloc.GetUsedBytes(), 0u);

    uint32_t as = 0;
    alloc.Alloc(4096, &as);
    EXPECT_EQ(alloc.GetUsedBytes(), 4096u);
}

// ---------------------------------------------------------------------------
// Edge cases
// ---------------------------------------------------------------------------
TEST(BuddyAllocator, FreeNegativeOffsetNoop) {
    BuddyAllocator alloc(64 * 1024, 4096);
    // Should not crash.
    alloc.Free(-1, 4096);
    EXPECT_DOUBLE_EQ(alloc.GetUsageRatio(), 0.0);
}

TEST(BuddyAllocator, GetPageSize) {
    BuddyAllocator alloc(64 * 1024, 4096);
    EXPECT_EQ(alloc.GetPageSize(), 4096u);
}

// ---------------------------------------------------------------------------
// Variable-size allocation
// ---------------------------------------------------------------------------
TEST(BuddyAllocator, VariableSizeAlloc) {
    // 64 KB, 4KB page
    BuddyAllocator alloc(64 * 1024, 4096);

    // Alloc 8KB -> needs 2 pages -> buddy order 1 -> alloc_size = 8KB
    uint32_t as1 = 0;
    int64_t off1 = alloc.Alloc(8000, &as1);
    ASSERT_GE(off1, 0);
    EXPECT_EQ(as1, 8192u); // 2 pages * 4096

    // Alloc 16KB -> needs 4 pages -> buddy order 2 -> alloc_size = 16KB
    uint32_t as2 = 0;
    int64_t off2 = alloc.Alloc(16000, &as2);
    ASSERT_GE(off2, 0);
    EXPECT_EQ(as2, 16384u); // 4 pages * 4096

    // Free and verify no leak
    alloc.Free(off1, as1);
    alloc.Free(off2, as2);
    EXPECT_DOUBLE_EQ(alloc.GetUsageRatio(), 0.0);
}

TEST(BuddyAllocator, ComputeAllocSize) {
    BuddyAllocator alloc(64 * 1024, 4096);

    // 100 bytes -> 1 page -> alloc_size = 4096
    EXPECT_EQ(alloc.ComputeAllocSize(100), 4096u);

    // 5000 bytes -> 2 pages (ceil) -> alloc_size = 8192
    EXPECT_EQ(alloc.ComputeAllocSize(5000), 8192u);

    // 4096 bytes -> 1 page -> alloc_size = 4096
    EXPECT_EQ(alloc.ComputeAllocSize(4096), 4096u);

    // 8192 bytes -> 2 pages -> alloc_size = 8192
    EXPECT_EQ(alloc.ComputeAllocSize(8192), 8192u);

    // 8193 bytes -> 3 pages -> buddy order 2 -> 4 pages = 16384
    EXPECT_EQ(alloc.ComputeAllocSize(8193), 16384u);
}

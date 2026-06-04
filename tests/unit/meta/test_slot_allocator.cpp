#include <gtest/gtest.h>
#include <vector>
#include <unordered_set>
#include <thread>
#include <atomic>

#include "src/common/slot_allocator.h"

using namespace falconkv;

// ---------------------------------------------------------------------------
// Alloc returns valid offset (explicit slot size)
// ---------------------------------------------------------------------------
TEST(SlotAllocator, AllocReturnsValidOffset) {
    // 1 MB total, 4KB slot size
    SlotAllocator alloc(1024 * 1024, 4096);

    uint32_t alloc_size = 0;
    int64_t offset = alloc.Alloc(4096, &alloc_size);
    EXPECT_GE(offset, 0);
    EXPECT_EQ(offset % 4096, 0); // slot aligned
    EXPECT_EQ(alloc_size, 4096u);
}

TEST(SlotAllocator, AllocReturnsPageAlignedOffset) {
    SlotAllocator alloc(1024 * 1024, 4096);

    for (int i = 0; i < 10; ++i) {
        uint32_t alloc_size = 0;
        int64_t offset = alloc.Alloc(4096, &alloc_size);
        ASSERT_GE(offset, 0) << "Iteration " << i;
        EXPECT_EQ(offset % 4096, 0) << "Iteration " << i;
    }
}

// ---------------------------------------------------------------------------
// Alloc until exhaustion (explicit mode)
// ---------------------------------------------------------------------------
TEST(SlotAllocator, AllocUntilExhaustion) {
    // 64 KB total, 4KB slot size -> 16 slots total
    uint64_t total = 64 * 1024;
    SlotAllocator alloc(total, 4096);

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
// Free allows re-allocation
// ---------------------------------------------------------------------------
TEST(SlotAllocator, FreeAllowsRealloc) {
    // 64 KB, 4KB slot = 16 slots
    SlotAllocator alloc(64 * 1024, 4096);

    // Allocate all slots.
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

TEST(SlotAllocator, FreeAndUsageZero) {
    SlotAllocator alloc(64 * 1024, 4096);

    uint32_t as0 = 0, as1 = 0;
    int64_t off0 = alloc.Alloc(4096, &as0);
    int64_t off1 = alloc.Alloc(4096, &as1);
    ASSERT_GE(off0, 0);
    ASSERT_GE(off1, 0);

    // Free both.
    alloc.Free(off0, as0);
    alloc.Free(off1, as1);

    // Usage should be zero now.
    EXPECT_DOUBLE_EQ(alloc.GetUsageRatio(), 0.0);
}

// ---------------------------------------------------------------------------
// GetUsageRatio
// ---------------------------------------------------------------------------
TEST(SlotAllocator, GetUsageRatioBasic) {
    SlotAllocator alloc(64 * 1024, 4096);

    EXPECT_DOUBLE_EQ(alloc.GetUsageRatio(), 0.0);

    uint32_t as = 0;
    alloc.Alloc(4096, &as);
    // 1 slot out of 16 = 1/16
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
TEST(SlotAllocator, RepeatedAllocFreeNoLeak) {
    SlotAllocator alloc(64 * 1024, 4096);

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

TEST(SlotAllocator, AllocNFreeAllUsageZero) {
    SlotAllocator alloc(64 * 1024, 4096);

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
TEST(SlotAllocator, GetTotalBytes) {
    SlotAllocator alloc(1024 * 1024, 4096);
    EXPECT_EQ(alloc.GetTotalBytes(), 1024u * 1024u);
}

TEST(SlotAllocator, GetUsedBytes) {
    SlotAllocator alloc(64 * 1024, 4096);
    EXPECT_EQ(alloc.GetUsedBytes(), 0u);

    uint32_t as = 0;
    alloc.Alloc(4096, &as);
    EXPECT_EQ(alloc.GetUsedBytes(), 4096u);
}

// ---------------------------------------------------------------------------
// Edge cases
// ---------------------------------------------------------------------------
TEST(SlotAllocator, FreeNegativeOffsetNoop) {
    SlotAllocator alloc(64 * 1024, 4096);
    // Should not crash.
    alloc.Free(-1, 4096);
    EXPECT_DOUBLE_EQ(alloc.GetUsageRatio(), 0.0);
}

TEST(SlotAllocator, GetPageSize) {
    SlotAllocator alloc(64 * 1024, 4096);
    EXPECT_EQ(alloc.GetPageSize(), 4096u);
}

// ---------------------------------------------------------------------------
// ComputeAllocSize - always returns slot_size after init
// ---------------------------------------------------------------------------
TEST(SlotAllocator, ComputeAllocSize) {
    SlotAllocator alloc(64 * 1024, 4096);

    // All sizes should return slot_size (4096)
    EXPECT_EQ(alloc.ComputeAllocSize(100), 4096u);
    EXPECT_EQ(alloc.ComputeAllocSize(5000), 4096u);
    EXPECT_EQ(alloc.ComputeAllocSize(4096), 4096u);
    EXPECT_EQ(alloc.ComputeAllocSize(1), 4096u);
    EXPECT_EQ(alloc.ComputeAllocSize(0), 4096u);
}

// ---------------------------------------------------------------------------
// Size exceeding slot_size should fail (explicit mode)
// ---------------------------------------------------------------------------
TEST(SlotAllocator, AllocExceedingSlotSize) {
    SlotAllocator alloc(64 * 1024, 4096);

    // Requesting more than slot_size should fail
    int64_t off = alloc.Alloc(8192);
    EXPECT_LT(off, 0);
}

// ---------------------------------------------------------------------------
// Auto-detect from first Alloc (slot_size=0)
// ---------------------------------------------------------------------------
TEST(SlotAllocator, AutoDetectSlotSize) {
    // 100MB total, slot_size auto-detect from first Alloc -> 2MB -> 50 slots
    SlotAllocator alloc(100 * 1024 * 1024, 0);
    // Before first alloc, GetPageSize() returns 0
    EXPECT_EQ(alloc.GetPageSize(), 0u);

    uint32_t as = 0;
    int64_t off = alloc.Alloc(2 * 1024 * 1024, &as);
    ASSERT_GE(off, 0);
    EXPECT_EQ(as, 2u * 1024 * 1024);
    EXPECT_EQ(off % (2 * 1024 * 1024), 0);

    // Now GetPageSize should reflect the auto-detected size
    EXPECT_EQ(alloc.GetPageSize(), 2u * 1024 * 1024);
}

// ===========================================================================
// NEW TESTS
// ===========================================================================

// ---------------------------------------------------------------------------
// 1. AutoDetectFromFirstAlloc - basic lazy init flow
// ---------------------------------------------------------------------------
TEST(SlotAllocator, AutoDetectFromFirstAlloc) {
    SlotAllocator alloc(64 * 1024, 0);

    // Before init: total_bytes unchanged, used_bytes=0, usage=0
    EXPECT_EQ(alloc.GetTotalBytes(), 64u * 1024u);
    EXPECT_EQ(alloc.GetUsedBytes(), 0u);
    EXPECT_DOUBLE_EQ(alloc.GetUsageRatio(), 0.0);

    // First alloc with size=4096 triggers auto-detect
    uint32_t as = 0;
    int64_t off = alloc.Alloc(4096, &as);
    ASSERT_GE(off, 0);
    EXPECT_EQ(as, 4096u);
    EXPECT_EQ(off, 0);  // first slot, offset 0

    // Now initialized: total_bytes = 16 * 4096 = 64KB (unchanged in this case)
    EXPECT_EQ(alloc.GetTotalBytes(), 64u * 1024u);
    EXPECT_EQ(alloc.GetUsedBytes(), 4096u);
    EXPECT_EQ(alloc.GetPageSize(), 4096u);

    // Second alloc gets next bump slot
    int64_t off2 = alloc.Alloc(4096, &as);
    ASSERT_GE(off2, 0);
    EXPECT_EQ(off2, 4096);  // second slot, offset 4096
}

// ---------------------------------------------------------------------------
// 2. AutoDetectRejectsDifferentSize - different size returns -1
// ---------------------------------------------------------------------------
TEST(SlotAllocator, AutoDetectRejectsDifferentSize) {
    SlotAllocator alloc(64 * 1024, 0);

    // First alloc sets slot_size=4096
    uint32_t as = 0;
    ASSERT_GE(alloc.Alloc(4096, &as), 0);

    // Different size should fail
    int64_t off = alloc.Alloc(8192);
    EXPECT_LT(off, 0);

    // Also smaller size should fail (exact match required)
    off = alloc.Alloc(2048);
    EXPECT_LT(off, 0);

    // Same size should still work
    off = alloc.Alloc(4096);
    EXPECT_GE(off, 0);
}

// ---------------------------------------------------------------------------
// 3. AutoDetectRejectsSizeZero - size=0 cannot trigger init
// ---------------------------------------------------------------------------
TEST(SlotAllocator, AutoDetectRejectsSizeZero) {
    SlotAllocator alloc(64 * 1024, 0);

    int64_t off = alloc.Alloc(0);
    EXPECT_LT(off, 0);

    // Still not initialized, can still detect from a valid size
    uint32_t as = 0;
    off = alloc.Alloc(4096, &as);
    ASSERT_GE(off, 0);
    EXPECT_EQ(as, 4096u);
}

// ---------------------------------------------------------------------------
// 4. AutoDetectAllocUntilExhaustion - alloc until full, free, realloc
// ---------------------------------------------------------------------------
TEST(SlotAllocator, AutoDetectAllocUntilExhaustion) {
    // 64KB total, auto-detect slot_size=4096 -> 16 slots
    SlotAllocator alloc(64 * 1024, 0);

    std::vector<int64_t> offsets;
    for (int i = 0; i < 16; ++i) {
        int64_t off = alloc.Alloc(4096);
        ASSERT_GE(off, 0) << "Allocation " << i;
        offsets.push_back(off);
    }

    // Exhausted
    EXPECT_LT(alloc.Alloc(4096), 0);

    // Free 4 slots
    for (int i = 0; i < 4; ++i) {
        alloc.Free(offsets[i], 4096);
    }

    // Can allocate 4 more (from free_stack now)
    for (int i = 0; i < 4; ++i) {
        int64_t off = alloc.Alloc(4096);
        EXPECT_GE(off, 0) << "Re-allocation " << i;
    }

    // Exhausted again
    EXPECT_LT(alloc.Alloc(4096), 0);
}

// ---------------------------------------------------------------------------
// 5. ExplicitSlotSizeLegacyBehavior - explicit mode keeps size <= slot_size_
// ---------------------------------------------------------------------------
TEST(SlotAllocator, ExplicitSlotSizeLegacyBehavior) {
    SlotAllocator alloc(64 * 1024, 4096);

    // size < slot_size is allowed in explicit mode
    int64_t off = alloc.Alloc(100);
    EXPECT_GE(off, 0);

    off = alloc.Alloc(4096);
    EXPECT_GE(off, 0);

    // size > slot_size is rejected
    off = alloc.Alloc(4097);
    EXPECT_LT(off, 0);
}

// ---------------------------------------------------------------------------
// 6. FreeBeforeInitIsNoop - uninitialized Free doesn't crash
// ---------------------------------------------------------------------------
TEST(SlotAllocator, FreeBeforeInitIsNoop) {
    SlotAllocator alloc(64 * 1024, 0);

    // These should not crash
    alloc.Free(0, 4096);
    alloc.Free(4096, 4096);
    alloc.Free(-1, 4096);

    EXPECT_DOUBLE_EQ(alloc.GetUsageRatio(), 0.0);
    EXPECT_EQ(alloc.GetUsedBytes(), 0u);

    // Can still initialize normally after no-op frees
    uint32_t as = 0;
    int64_t off = alloc.Alloc(4096, &as);
    ASSERT_GE(off, 0);
    EXPECT_EQ(as, 4096u);
}

// ---------------------------------------------------------------------------
// 7. AutoDetectConcurrency - multi-threaded first Alloc
// ---------------------------------------------------------------------------
TEST(SlotAllocator, AutoDetectConcurrency) {
    SlotAllocator alloc(1024 * 1024, 0);  // auto-detect

    const int N = 8;
    std::vector<int64_t> results(N, -2);
    std::vector<std::thread> threads;

    for (int i = 0; i < N; ++i) {
        threads.emplace_back([&alloc, &results, i]() {
            results[i] = alloc.Alloc(4096);
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    // All allocations should succeed
    for (int i = 0; i < N; ++i) {
        EXPECT_GE(results[i], 0) << "Thread " << i << " failed";
    }

    // All offsets should be unique
    std::unordered_set<int64_t> unique(results.begin(), results.end());
    EXPECT_EQ(unique.size(), static_cast<size_t>(N));

    // All should be 4096-aligned
    for (int i = 0; i < N; ++i) {
        EXPECT_EQ(results[i] % 4096, 0) << "Thread " << i;
    }

    EXPECT_EQ(alloc.GetPageSize(), 4096u);
    EXPECT_EQ(alloc.GetUsedBytes(), N * 4096u);
}

// ---------------------------------------------------------------------------
// 8. BumpAllocatorThenFreeList - verify virgin bump then free_stack reuse
// ---------------------------------------------------------------------------
TEST(SlotAllocator, BumpAllocatorThenFreeList) {
    // 16KB total, auto-detect 4096 -> 4 slots
    SlotAllocator alloc(16 * 1024, 0);

    // Allocate all 4 slots from virgin region (bump)
    int64_t off0 = alloc.Alloc(4096);
    int64_t off1 = alloc.Alloc(4096);
    int64_t off2 = alloc.Alloc(4096);
    int64_t off3 = alloc.Alloc(4096);

    // Bump allocator gives sequential offsets
    EXPECT_EQ(off0, 0);
    EXPECT_EQ(off1, 4096);
    EXPECT_EQ(off2, 8192);
    EXPECT_EQ(off3, 12288);

    // Exhausted
    EXPECT_LT(alloc.Alloc(4096), 0);

    // Free slot 1 and slot 3
    alloc.Free(off1, 4096);  // slot index 1
    alloc.Free(off3, 4096);  // slot index 3

    // Next allocs should come from free_stack (LIFO: slot 3 first, then slot 1)
    int64_t reused_a = alloc.Alloc(4096);
    EXPECT_EQ(reused_a, 12288);  // slot 3 (last freed, LIFO)

    int64_t reused_b = alloc.Alloc(4096);
    EXPECT_EQ(reused_b, 4096);  // slot 1

    // Exhausted again
    EXPECT_LT(alloc.Alloc(4096), 0);
}

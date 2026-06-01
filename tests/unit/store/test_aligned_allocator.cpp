#include <gtest/gtest.h>
#include <cstdint>
#include <cstdlib>

#include "src/common/aligned_allocator.h"

using namespace falconkv;

// ---------------------------------------------------------------------------
// Allocate returns aligned pointer
// ---------------------------------------------------------------------------
TEST(AlignedAllocator, AllocateReturnsAlignedPointer) {
    const size_t alignment = 512;
    void* ptr = AlignedAllocator::Allocate(alignment, 4096);
    ASSERT_NE(ptr, nullptr);
    EXPECT_TRUE(AlignedAllocator::IsAligned(ptr, alignment));
    AlignedAllocator::Free(ptr);
}

TEST(AlignedAllocator, AllocateWith4096Alignment) {
    const size_t alignment = 4096;
    void* ptr = AlignedAllocator::Allocate(alignment, 8192);
    ASSERT_NE(ptr, nullptr);
    EXPECT_TRUE(AlignedAllocator::IsAligned(ptr, alignment));
    AlignedAllocator::Free(ptr);
}

TEST(AlignedAllocator, MultipleAllocationsAllAligned) {
    const size_t alignment = 256;
    for (int i = 0; i < 100; ++i) {
        void* ptr = AlignedAllocator::Allocate(alignment, 1024);
        ASSERT_NE(ptr, nullptr);
        EXPECT_TRUE(AlignedAllocator::IsAligned(ptr, alignment))
            << "Allocation " << i << " not aligned";
        AlignedAllocator::Free(ptr);
    }
}

// ---------------------------------------------------------------------------
// Free works (no crash, no leak under sanitizers)
// ---------------------------------------------------------------------------
TEST(AlignedAllocator, FreeNullptrNoop) {
    // Freeing nullptr should be safe (delegates to free()).
    AlignedAllocator::Free(nullptr);
}

TEST(AlignedAllocator, AllocateAndFreeRepeated) {
    for (int i = 0; i < 1000; ++i) {
        void* ptr = AlignedAllocator::Allocate(64, 128);
        ASSERT_NE(ptr, nullptr);
        AlignedAllocator::Free(ptr);
    }
}

// ---------------------------------------------------------------------------
// IsAligned checks
// ---------------------------------------------------------------------------
TEST(AlignedAllocator, IsAlignedTrue) {
    EXPECT_TRUE(AlignedAllocator::IsAligned(reinterpret_cast<void*>(0), 4096));
    EXPECT_TRUE(AlignedAllocator::IsAligned(reinterpret_cast<void*>(4096), 4096));
    EXPECT_TRUE(AlignedAllocator::IsAligned(reinterpret_cast<void*>(8192), 4096));
}

TEST(AlignedAllocator, IsAlignedFalse) {
    EXPECT_FALSE(AlignedAllocator::IsAligned(reinterpret_cast<void*>(1), 4096));
    EXPECT_FALSE(AlignedAllocator::IsAligned(reinterpret_cast<void*>(100), 512));
    EXPECT_FALSE(AlignedAllocator::IsAligned(reinterpret_cast<void*>(4095), 4096));
}

TEST(AlignedAllocator, IsAlignedPowerOfTwo) {
    EXPECT_TRUE(AlignedAllocator::IsAligned(reinterpret_cast<void*>(1024), 1024));
    EXPECT_TRUE(AlignedAllocator::IsAligned(reinterpret_cast<void*>(2048), 1024));
    EXPECT_FALSE(AlignedAllocator::IsAligned(reinterpret_cast<void*>(1025), 1024));
}

// ---------------------------------------------------------------------------
// AlignUp calculation
// ---------------------------------------------------------------------------
TEST(AlignedAllocator, AlignUpAlreadyAligned) {
    EXPECT_EQ(AlignedAllocator::AlignUp(0, 4096), 0u);
    EXPECT_EQ(AlignedAllocator::AlignUp(4096, 4096), 4096u);
    EXPECT_EQ(AlignedAllocator::AlignUp(8192, 4096), 8192u);
}

TEST(AlignedAllocator, AlignUpNeedsAlignment) {
    EXPECT_EQ(AlignedAllocator::AlignUp(1, 4096), 4096u);
    EXPECT_EQ(AlignedAllocator::AlignUp(4095, 4096), 4096u);
    EXPECT_EQ(AlignedAllocator::AlignUp(100, 64), 128u);
    EXPECT_EQ(AlignedAllocator::AlignUp(5000, 4096), 8192u);
}

TEST(AlignedAllocator, AlignUpVariousAlignments) {
    EXPECT_EQ(AlignedAllocator::AlignUp(5, 4), 8u);
    EXPECT_EQ(AlignedAllocator::AlignUp(8, 4), 8u);
    EXPECT_EQ(AlignedAllocator::AlignUp(9, 4), 12u);
    EXPECT_EQ(AlignedAllocator::AlignUp(0, 512), 0u);
    EXPECT_EQ(AlignedAllocator::AlignUp(1, 512), 512u);
}

TEST(AlignedAllocator, AlignUpPreservesValue) {
    // Ensure AlignUp never decreases the input.
    for (size_t val = 0; val < 100; ++val) {
        size_t aligned = AlignedAllocator::AlignUp(val, 16);
        EXPECT_GE(aligned, val);
        EXPECT_EQ(aligned % 16, 0u);
    }
}

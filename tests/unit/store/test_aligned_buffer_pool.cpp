#include <gtest/gtest.h>
#include <vector>
#include <cstdlib>
#include <cstdint>

#include "src/store/aligned_buffer_pool.h"
#include "src/common/aligned_allocator.h"

using namespace falconkv;

// ---------------------------------------------------------------------------
// Get() from pool returns all pre-allocated buffers
// ---------------------------------------------------------------------------
TEST(AlignedBufferPoolTest, GetFromPool) {
    const size_t pool_size = 4;
    const size_t buf_size = 4096;
    const size_t alignment = 512;

    AlignedBufferPool pool(buf_size, alignment, pool_size);

    std::vector<void*> bufs;
    for (size_t i = 0; i < pool_size; ++i) {
        void* buf = pool.Get();
        ASSERT_NE(buf, nullptr) << "Get() " << i << " returned nullptr";
        bufs.push_back(buf);
    }

    // All pointers should be distinct
    for (size_t i = 0; i < bufs.size(); ++i) {
        for (size_t j = i + 1; j < bufs.size(); ++j) {
            EXPECT_NE(bufs[i], bufs[j]) << "Duplicate buffer at index " << i << " and " << j;
        }
    }

    // Clean up
    for (void* buf : bufs) {
        pool.Put(buf);
    }
}

// ---------------------------------------------------------------------------
// Pool exhaustion triggers dynamic allocation
// ---------------------------------------------------------------------------
TEST(AlignedBufferPoolTest, PoolExhaustionDynamicAlloc) {
    const size_t pool_size = 2;
    const size_t buf_size = 4096;
    const size_t alignment = 512;

    AlignedBufferPool pool(buf_size, alignment, pool_size);

    void* b1 = pool.Get();
    ASSERT_NE(b1, nullptr);

    void* b2 = pool.Get();
    ASSERT_NE(b2, nullptr);

    // Pool is now exhausted — third Get() should dynamically allocate.
    void* b3 = pool.Get();
    ASSERT_NE(b3, nullptr) << "Dynamic allocation after pool exhaustion failed";

    // All three must be distinct.
    EXPECT_NE(b1, b2);
    EXPECT_NE(b2, b3);
    EXPECT_NE(b1, b3);

    pool.Put(b1);
    pool.Put(b2);
    pool.Put(b3);
}

// ---------------------------------------------------------------------------
// Put() returns buffers to the pool for reuse
// ---------------------------------------------------------------------------
TEST(AlignedBufferPoolTest, PutAndReuse) {
    const size_t pool_size = 2;
    const size_t buf_size = 4096;
    const size_t alignment = 512;

    AlignedBufferPool pool(buf_size, alignment, pool_size);

    // Drain the pool.
    void* b1 = pool.Get();
    void* b2 = pool.Get();
    ASSERT_NE(b1, nullptr);
    ASSERT_NE(b2, nullptr);

    // Return both.
    pool.Put(b1);
    pool.Put(b2);

    // Get again — should reuse the returned buffers.
    void* b3 = pool.Get();
    void* b4 = pool.Get();
    ASSERT_NE(b3, nullptr);
    ASSERT_NE(b4, nullptr);

    // b3 and b4 must be the same pointers as b1/b2 (in LIFO order).
    EXPECT_TRUE(b3 == b2 || b3 == b1);
    EXPECT_TRUE(b4 == b2 || b4 == b1);
    EXPECT_NE(b3, b4);

    pool.Put(b3);
    pool.Put(b4);
}

// ---------------------------------------------------------------------------
// Every returned pointer satisfies the requested alignment
// ---------------------------------------------------------------------------
TEST(AlignedBufferPoolTest, AlignmentVerification) {
    const size_t pool_size = 4;
    const size_t buf_size = 4096;
    const size_t alignment = 512;

    AlignedBufferPool pool(buf_size, alignment, pool_size);

    for (size_t i = 0; i < pool_size + 2; ++i) {
        void* buf = pool.Get();
        ASSERT_NE(buf, nullptr);
        EXPECT_TRUE(AlignedAllocator::IsAligned(buf, alignment))
            << "Buffer " << i << " not aligned to " << alignment;
        pool.Put(buf);
    }
}

// ---------------------------------------------------------------------------
// Put(nullptr) is a no-op and does not crash
// ---------------------------------------------------------------------------
TEST(AlignedBufferPoolTest, PutNullptrIsNoop) {
    AlignedBufferPool pool(4096, 512, 2);

    // Should not crash or corrupt internal state.
    pool.Put(nullptr);

    // Verify the pool still works normally after Put(nullptr).
    void* buf = pool.Get();
    ASSERT_NE(buf, nullptr);
    pool.Put(buf);
}

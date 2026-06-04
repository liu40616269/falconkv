#include <gtest/gtest.h>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include "src/store/io_uring_engine.h"
#include "src/common/aligned_allocator.h"

using namespace falconkv;

namespace {

std::string GetTestFile() {
    static int counter = 0;
    return "/tmp/falconkv_test_io_uring_" + std::to_string(getpid()) +
           "_" + std::to_string(counter++) + ".dat";
}

class IOUringEngineTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_file_ = GetTestFile();
        fd_ = open(test_file_.c_str(), O_CREAT | O_RDWR | O_DIRECT,
                    0644);
        if (fd_ < 0) {
            // Some environments may not support O_DIRECT on tmpfs;
            // fall back to normal open.
            fd_ = open(test_file_.c_str(), O_CREAT | O_RDWR, 0644);
            direct_io_ = false;
        } else {
            direct_io_ = true;
        }
        ASSERT_GE(fd_, 0);

        // Preallocate 4MB
        int ret = ftruncate(fd_, 4 * 1024 * 1024);
        if (ret) {
          /* code */
        }

        page_size_ = 4096;
    }

    void TearDown() override {
        if (fd_ >= 0) {
            close(fd_);
        }
        unlink(test_file_.c_str());
    }

    std::string test_file_;
    int fd_ = -1;
    bool direct_io_ = true;
    uint32_t page_size_ = 4096;
};

} // anonymous namespace

// ---------------------------------------------------------------------------
// Init / Close lifecycle
// ---------------------------------------------------------------------------
TEST_F(IOUringEngineTest, InitClose) {
    IOUringEngine engine;
    bool ok = engine.Init({.queue_depth = 32});
    // io_uring may or may not be available depending on the system.
    if (ok) {
        EXPECT_TRUE(engine.available());
        engine.Close();
        EXPECT_FALSE(engine.available());
    } else {
        EXPECT_FALSE(engine.available());
    }
}

// ---------------------------------------------------------------------------
// BatchWrite + BatchRead roundtrip (aligned, fast path)
// ---------------------------------------------------------------------------
TEST_F(IOUringEngineTest, BatchWriteReadAlignedRoundtrip) {
    IOUringEngine engine;
    if (!engine.Init({.queue_depth = 32})) {
        GTEST_SKIP() << "io_uring not available on this system";
    }

    const int num_items = 4;
    std::vector<void*> write_bufs(num_items);
    std::vector<void*> read_bufs(num_items);

    for (int i = 0; i < num_items; ++i) {
        write_bufs[i] = AlignedAllocator::Allocate(page_size_, page_size_);
        read_bufs[i] = AlignedAllocator::Allocate(page_size_, page_size_);
        ASSERT_NE(write_bufs[i], nullptr);
        ASSERT_NE(read_bufs[i], nullptr);
        memset(write_bufs[i], static_cast<int>('A' + i), page_size_);
        memset(read_bufs[i], 0, page_size_);
    }

    // Write
    std::vector<UringIORequest> write_reqs(num_items);
    for (int i = 0; i < num_items; ++i) {
        write_reqs[i].offset = static_cast<uint64_t>(i) * page_size_;
        write_reqs[i].size = page_size_;
        write_reqs[i].buffer = write_bufs[i];
    }
    auto write_results = engine.BatchWrite(fd_, write_reqs);
    EXPECT_EQ(write_results.size(), static_cast<size_t>(num_items));
    for (const auto& r : write_results) {
        EXPECT_TRUE(r.status.ok()) << "Write failed: " << r.status.ToString();
    }

    // Read
    std::vector<UringIORequest> read_reqs(num_items);
    for (int i = 0; i < num_items; ++i) {
        read_reqs[i].offset = static_cast<uint64_t>(i) * page_size_;
        read_reqs[i].size = page_size_;
        read_reqs[i].buffer = read_bufs[i];
    }
    auto read_results = engine.BatchRead(fd_, read_reqs);
    EXPECT_EQ(read_results.size(), static_cast<size_t>(num_items));
    for (int i = 0; i < num_items; ++i) {
        EXPECT_TRUE(read_results[i].status.ok())
            << "Read failed: " << read_results[i].status.ToString();
        EXPECT_EQ(memcmp(write_bufs[i], read_bufs[i], page_size_), 0)
            << "Data mismatch at index " << i;
    }

    for (int i = 0; i < num_items; ++i) {
        AlignedAllocator::Free(write_bufs[i]);
        AlignedAllocator::Free(read_bufs[i]);
    }

    engine.Close();
}

// ---------------------------------------------------------------------------
// Empty batch
// ---------------------------------------------------------------------------
TEST_F(IOUringEngineTest, EmptyBatch) {
    IOUringEngine engine;
    if (!engine.Init({.queue_depth = 32})) {
        GTEST_SKIP() << "io_uring not available on this system";
    }

    std::vector<UringIORequest> empty;
    auto write_results = engine.BatchWrite(fd_, empty);
    EXPECT_EQ(write_results.size(), 0u);

    auto read_results = engine.BatchRead(fd_, empty);
    EXPECT_EQ(read_results.size(), 0u);

    engine.Close();
}

// ---------------------------------------------------------------------------
// Large batch exceeding queue_depth (sub-batch submission)
// ---------------------------------------------------------------------------
TEST_F(IOUringEngineTest, LargeBatchSubmitsInSubBatches) {
    IOUringEngine engine;
    if (!engine.Init({.queue_depth = 16})) {
        GTEST_SKIP() << "io_uring not available on this system";
    }

    const int num_items = 64; // > queue_depth=16
    std::vector<void*> bufs(num_items);
    for (int i = 0; i < num_items; ++i) {
        bufs[i] = AlignedAllocator::Allocate(page_size_, page_size_);
        ASSERT_NE(bufs[i], nullptr);
        memset(bufs[i], static_cast<int>(i % 256), page_size_);
    }

    std::vector<UringIORequest> write_reqs(num_items);
    for (int i = 0; i < num_items; ++i) {
        write_reqs[i].offset = static_cast<uint64_t>(i) * page_size_;
        write_reqs[i].size = page_size_;
        write_reqs[i].buffer = bufs[i];
    }
    auto write_results = engine.BatchWrite(fd_, write_reqs);
    for (const auto& r : write_results) {
        EXPECT_TRUE(r.status.ok()) << "Write failed: " << r.status.ToString();
    }

    // Verify by reading back
    std::vector<void*> read_bufs(num_items);
    for (int i = 0; i < num_items; ++i) {
        read_bufs[i] = AlignedAllocator::Allocate(page_size_, page_size_);
    }
    std::vector<UringIORequest> read_reqs(num_items);
    for (int i = 0; i < num_items; ++i) {
        read_reqs[i].offset = static_cast<uint64_t>(i) * page_size_;
        read_reqs[i].size = page_size_;
        read_reqs[i].buffer = read_bufs[i];
    }
    auto read_results = engine.BatchRead(fd_, read_reqs);
    for (int i = 0; i < num_items; ++i) {
        EXPECT_TRUE(read_results[i].status.ok());
        EXPECT_EQ(memcmp(bufs[i], read_bufs[i], page_size_), 0)
            << "Data mismatch at index " << i;
    }

    for (int i = 0; i < num_items; ++i) {
        AlignedAllocator::Free(bufs[i]);
        AlignedAllocator::Free(read_bufs[i]);
    }

    engine.Close();
}

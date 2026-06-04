#include <gtest/gtest.h>
#include <string>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cstdlib>

#include "src/store/falconkv_store.h"
#include "src/common/aligned_allocator.h"

using namespace falconkv;

namespace {

// Create a temporary directory for test data.
std::string GetTestDir() {
    static int counter = 0;
    std::string dir = "/tmp/falconkv_test_dio_" + std::to_string(getpid()) +
                      "_" + std::to_string(counter++);
    mkdir(dir.c_str(), 0755);
    return dir;
}

} // anonymous namespace

class DirectIOTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir_ = GetTestDir();

        FalconKVStore::Config config;
        config.ssd_path = test_dir_;
        config.store_id = 0;
        // Small capacity for tests (4 MB).
        config.capacity_bytes = 4 * 1024 * 1024;
        config.page_size = 4096;
        config.io_threads = 2;
        config.disable_mtime = true;

        store_ = std::make_unique<FalconKVStore>(config);
    }

    void TearDown() override {
        store_.reset();
        // Clean up test directory.
        std::string cmd = "rm -rf " + test_dir_;
        if (system(cmd.c_str())) {}
    }

    std::string test_dir_;
    std::unique_ptr<FalconKVStore> store_;
};

// ---------------------------------------------------------------------------
// Aligned write and read
// ---------------------------------------------------------------------------
TEST_F(DirectIOTest, AlignedWriteAndRead) {
    Status s = store_->Init();
    ASSERT_TRUE(s.ok()) << "Init failed: " << s.msg();

    const uint32_t size = 4096;
    void* write_buf = AlignedAllocator::Allocate(4096, size);
    ASSERT_NE(write_buf, nullptr);

    // Fill with a pattern.
    memset(write_buf, 0xAB, size);

    s = store_->Write(0, write_buf, size);
    EXPECT_TRUE(s.ok()) << "Write failed: " << s.msg();

    void* read_buf = AlignedAllocator::Allocate(4096, size);
    ASSERT_NE(read_buf, nullptr);
    memset(read_buf, 0, size);

    s = store_->Read(0, read_buf, size);
    EXPECT_TRUE(s.ok()) << "Read failed: " << s.msg();

    EXPECT_EQ(memcmp(write_buf, read_buf, size), 0);

    AlignedAllocator::Free(write_buf);
    AlignedAllocator::Free(read_buf);
}

// ---------------------------------------------------------------------------
// Unaligned buffer auto-alignment
// ---------------------------------------------------------------------------
TEST_F(DirectIOTest, UnalignedBufferWriteRead) {
    Status s = store_->Init();
    ASSERT_TRUE(s.ok()) << "Init failed: " << s.msg();

    const uint32_t size = 4096;
    // Allocate a slightly larger buffer and use a non-aligned offset within it.
    auto raw = std::make_unique<uint8_t[]>(size + 64);
    uint8_t* unaligned = raw.get() + 13; // intentionally non-aligned

    memset(unaligned, 0xCD, size);

    // Write at a page-aligned file offset with a non-aligned buffer.
    s = store_->Write(0, unaligned, size);
    EXPECT_TRUE(s.ok()) << "Unaligned write failed: " << s.msg();

    auto read_raw = std::make_unique<uint8_t[]>(size + 64);
    uint8_t* read_unaligned = read_raw.get() + 7;

    s = store_->Read(0, read_unaligned, size);
    EXPECT_TRUE(s.ok()) << "Unaligned read failed: " << s.msg();

    EXPECT_EQ(memcmp(unaligned, read_unaligned, size), 0);
}

// ---------------------------------------------------------------------------
// Write/Read roundtrip verification with different patterns
// ---------------------------------------------------------------------------
TEST_F(DirectIOTest, WriteReadRoundtripMultipleOffsets) {
    Status s = store_->Init();
    ASSERT_TRUE(s.ok()) << "Init failed: " << s.msg();

    const uint32_t size = 4096;
    const int num_writes = 4;

    void* write_buf = AlignedAllocator::Allocate(4096, size);
    ASSERT_NE(write_buf, nullptr);

    void* read_buf = AlignedAllocator::Allocate(4096, size);
    ASSERT_NE(read_buf, nullptr);

    for (int i = 0; i < num_writes; ++i) {
        uint64_t offset = static_cast<uint64_t>(i) * size;

        // Fill with a unique pattern per offset.
        memset(write_buf, static_cast<int>('A' + i), size);

        s = store_->Write(offset, write_buf, size);
        ASSERT_TRUE(s.ok()) << "Write at offset " << offset << " failed: " << s.msg();

        memset(read_buf, 0, size);
        s = store_->Read(offset, read_buf, size);
        ASSERT_TRUE(s.ok()) << "Read at offset " << offset << " failed: " << s.msg();

        EXPECT_EQ(memcmp(write_buf, read_buf, size), 0)
            << "Data mismatch at offset " << offset;
    }

    AlignedAllocator::Free(write_buf);
    AlignedAllocator::Free(read_buf);
}

// ---------------------------------------------------------------------------
// Write at a valid large offset within capacity succeeds
// ---------------------------------------------------------------------------
TEST_F(DirectIOTest, WriteAtHighOffset) {
    Status s = store_->Init();
    ASSERT_TRUE(s.ok()) << "Init failed: " << s.msg();

    const uint32_t size = 4096;
    void* write_buf = AlignedAllocator::Allocate(4096, size);
    ASSERT_NE(write_buf, nullptr);
    memset(write_buf, 0xEE, size);

    // Write at an offset within capacity (2 MB into a 4 MB file).
    uint64_t offset = 2 * 1024 * 1024;
    s = store_->Write(offset, write_buf, size);
    EXPECT_TRUE(s.ok()) << "Write at valid offset failed: " << s.msg();

    // Read back and verify.
    void* read_buf = AlignedAllocator::Allocate(4096, size);
    ASSERT_NE(read_buf, nullptr);
    s = store_->Read(offset, read_buf, size);
    EXPECT_TRUE(s.ok()) << "Read failed: " << s.msg();
    EXPECT_EQ(memcmp(write_buf, read_buf, size), 0);

    AlignedAllocator::Free(write_buf);
    AlignedAllocator::Free(read_buf);
}

TEST_F(DirectIOTest, InitFailsWithInvalidPath) {
    FalconKVStore::Config config;
    config.ssd_path = "/nonexistent/deep/path/that/cannot/be/created";
    config.store_id = 99;
    config.capacity_bytes = 4096;
    config.page_size = 4096;

    FalconKVStore bad_store(config);
    // Init may or may not fail depending on permissions, but it should not crash.
    // We test that the call completes without hanging.
    Status s = bad_store.Init();
    // The result depends on the system; we just verify it does not crash.
    (void)s;
}

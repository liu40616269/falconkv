#include <gtest/gtest.h>
#include <string>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <thread>

#include "src/store/falconkv_store.h"
#include "src/common/aligned_allocator.h"

using namespace falconkv;

namespace {

std::string GetTempDir() {
    static int counter = 0;
    std::string dir = "/tmp/falconkv_module_store_" + std::to_string(getpid()) +
                      "_" + std::to_string(counter++);
    mkdir(dir.c_str(), 0755);
    return dir;
}

class StoreEngineTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir_ = GetTempDir();
    }

    void TearDown() override {
        std::string cmd = "rm -rf " + test_dir_;
        if (system(cmd.c_str())) {}
    }

    FalconKVStore::Config MakeConfig() {
        FalconKVStore::Config config;
        config.ssd_path = test_dir_;
        config.store_id = 1;
        config.node_id = 1;
        config.capacity_bytes = 16 * 1024 * 1024; // 16 MB
        config.page_size = 4096;
        config.slot_size_bytes = 4096;  // 4KB slots match test data sizes
        config.disable_mtime = true;
        config.scheduler_enabled = false;
        return config;
    }

    std::string test_dir_;
};

} // anonymous namespace

// ---------------------------------------------------------------------------
// MT-S-001: BatchWrite + BatchRead with out-of-order offsets
// ---------------------------------------------------------------------------
TEST_F(StoreEngineTest, BatchWriteReadCorrectness) {
    FalconKVStore store(MakeConfig());
    ASSERT_TRUE(store.Init().ok());

    const uint32_t chunk = 4096;
    const int num_items = 4;

    // Write at offsets: chunk*3, chunk*0, chunk*2, chunk*1 (out of order).
    uint64_t offsets[] = {chunk * 3, chunk * 0, chunk * 2, chunk * 1};

    std::vector<void*> write_bufs;
    std::vector<WriteItem> write_items;
    for (int i = 0; i < num_items; ++i) {
        void* buf = AlignedAllocator::Allocate(4096, chunk);
        ASSERT_NE(buf, nullptr);
        memset(buf, static_cast<int>('A' + i), chunk);
        write_bufs.push_back(buf);
        write_items.push_back({offsets[i], buf, chunk});
    }

    Status s = store.BatchWrite(write_items);
    ASSERT_TRUE(s.ok()) << "BatchWrite failed: " << s.msg();

    // Read back in order and verify.
    for (int i = 0; i < num_items; ++i) {
        void* read_buf = AlignedAllocator::Allocate(4096, chunk);
        ASSERT_NE(read_buf, nullptr);

        ReadItem ri = {offsets[i], read_buf, chunk};
        s = store.BatchRead({ri});
        ASSERT_TRUE(s.ok()) << "BatchRead failed at offset " << offsets[i];

        uint8_t expected = static_cast<uint8_t>('A' + i);
        const uint8_t* data = static_cast<const uint8_t*>(read_buf);
        for (uint32_t j = 0; j < chunk; ++j) {
            ASSERT_EQ(data[j], expected) << "Mismatch at offset " << offsets[i] << " byte " << j;
        }

        AlignedAllocator::Free(read_buf);
    }

    for (void* buf : write_bufs) AlignedAllocator::Free(buf);
    store.Close();
}

// ---------------------------------------------------------------------------
// MT-S-002: Key-aware Put/Get roundtrip
// ---------------------------------------------------------------------------
TEST_F(StoreEngineTest, KeyAwarePutGet) {
    FalconKVStore store(MakeConfig());
    ASSERT_TRUE(store.Init().ok());

    const std::string key = "test_key_001";
    const std::vector<uint8_t> data(4096, 0xDD);

    auto put_result = store.Put(key, data.data(), data.size());
    ASSERT_TRUE(put_result.status.ok()) << "Put failed: " << put_result.status.msg();

    std::vector<uint8_t> read_buf(data.size(), 0);
    auto get_result = store.Get(key, read_buf.data(), read_buf.size());
    ASSERT_TRUE(get_result.status.ok()) << "Get failed: " << get_result.status.msg();
    EXPECT_EQ(memcmp(data.data(), read_buf.data(), data.size()), 0);

    store.Close();
}

// ---------------------------------------------------------------------------
// MT-S-003: BatchPut 100 keys, then Get each one
// ---------------------------------------------------------------------------
TEST_F(StoreEngineTest, BatchPutGet) {
    FalconKVStore store(MakeConfig());
    ASSERT_TRUE(store.Init().ok());

    const int N = 100;
    std::vector<std::string> keys;
    std::vector<const void*> data_ptrs;
    std::vector<uint32_t> sizes;
    std::vector<std::vector<uint8_t>> data_storage;

    for (int i = 0; i < N; ++i) {
        keys.push_back("batch_key_" + std::to_string(i));
        data_storage.emplace_back(4096, static_cast<uint8_t>(i % 256));
        data_ptrs.push_back(data_storage.back().data());
        sizes.push_back(4096);
    }

    auto results = store.BatchPut(keys, data_ptrs, sizes);
    for (int i = 0; i < N; ++i) {
        ASSERT_TRUE(results[i].status.ok())
            << "BatchPut[" << i << "] failed: " << results[i].status.msg();
    }

    // Verify each key individually.
    for (int i = 0; i < N; ++i) {
        std::vector<uint8_t> read_buf(4096, 0);
        auto get_result = store.Get(keys[i], read_buf.data(), read_buf.size());
        ASSERT_TRUE(get_result.status.ok())
            << "Get[" << i << "] failed: " << get_result.status.msg();
        EXPECT_EQ(memcmp(data_storage[i].data(), read_buf.data(), 4096), 0)
            << "Data mismatch for key " << keys[i];
    }

    store.Close();
}

// ---------------------------------------------------------------------------
// MT-S-004: Write-then-read consistency (immediate read)
// ---------------------------------------------------------------------------
TEST_F(StoreEngineTest, WriteReadConsistency) {
    FalconKVStore store(MakeConfig());
    ASSERT_TRUE(store.Init().ok());

    const uint32_t size = 4096;
    void* write_buf = AlignedAllocator::Allocate(4096, size);
    ASSERT_NE(write_buf, nullptr);

    // Fill with a recognizable pattern.
    for (uint32_t i = 0; i < size; ++i) {
        static_cast<uint8_t*>(write_buf)[i] = static_cast<uint8_t>(i);
    }

    Status s = store.Write(0, write_buf, size);
    ASSERT_TRUE(s.ok());

    void* read_buf = AlignedAllocator::Allocate(4096, size);
    ASSERT_NE(read_buf, nullptr);

    s = store.Read(0, read_buf, size);
    ASSERT_TRUE(s.ok());
    EXPECT_EQ(memcmp(write_buf, read_buf, size), 0);

    AlignedAllocator::Free(write_buf);
    AlignedAllocator::Free(read_buf);
    store.Close();
}

// ---------------------------------------------------------------------------
// MT-S-005: Concurrent Puts from 4 threads
// ---------------------------------------------------------------------------
TEST_F(StoreEngineTest, ConcurrentPuts) {
    FalconKVStore store(MakeConfig());
    ASSERT_TRUE(store.Init().ok());

    const int threads = 4;
    const int per_thread = 25;

    std::vector<std::thread> workers;
    std::atomic<int> success_count{0};

    for (int t = 0; t < threads; ++t) {
        workers.emplace_back([&, t]() {
            for (int i = 0; i < per_thread; ++i) {
                std::string key = "thread" + std::to_string(t) + "_key" + std::to_string(i);
                std::vector<uint8_t> data(4096, static_cast<uint8_t>(t * 50 + i));
                auto result = store.Put(key, data.data(), data.size());
                if (result.status.ok()) {
                    success_count.fetch_add(1);
                }
            }
        });
    }

    for (auto& w : workers) w.join();

    EXPECT_EQ(success_count.load(), threads * per_thread);

    // Spot-check a few keys.
    for (int t = 0; t < threads; ++t) {
        std::string key = "thread" + std::to_string(t) + "_key0";
        std::vector<uint8_t> buf(4096, 0);
        auto result = store.Get(key, buf.data(), buf.size());
        EXPECT_TRUE(result.status.ok()) << "Get failed for " << key;
    }

    store.Close();
}

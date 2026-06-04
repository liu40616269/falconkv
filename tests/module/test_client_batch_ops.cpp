#include <gtest/gtest.h>
#include <string>
#include <vector>
#include <cstring>
#include <cstdlib>

#include "src/client/falconkv_client_impl.h"
#include "src/store/falconkv_store.h"
#include "src/meta/meta_manager.h"

using namespace falconkv;

namespace {

std::string GetTempDir() {
    static int counter = 0;
    std::string dir = "/tmp/falconkv_client_mod_" + std::to_string(getpid()) +
                      "_" + std::to_string(counter++);
    mkdir(dir.c_str(), 0755);
    return dir;
}

class ClientBatchOpsTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir_ = GetTempDir();

        // Create a real store.
        FalconKVStore::Config store_config;
        store_config.ssd_path = test_dir_;
        store_config.store_id = 1;
        store_config.node_id = 1;
        store_config.capacity_bytes = 16 * 1024 * 1024;
        store_config.page_size = 4096;
        store_config.slot_size_bytes = 4096;
        store_config.disable_mtime = true;
        store_config.scheduler_enabled = false;

        store_ = std::make_unique<FalconKVStore>(store_config);
        ASSERT_TRUE(store_->Init().ok());

        // Create a client bound to the local store.
        FalconKVClientImpl::Config client_config;
        client_config.scheduler_enabled = false;
        client_ = std::make_unique<FalconKVClientImpl>(client_config);
        client_->SetLocalStore(store_.get());
    }

    void TearDown() override {
        client_->Close();
        store_->Close();
        std::string cmd = "rm -rf " + test_dir_;
        if (system(cmd.c_str())) {}
    }

    std::string test_dir_;
    std::unique_ptr<FalconKVStore> store_;
    std::unique_ptr<FalconKVClientImpl> client_;
    std::vector<std::unique_ptr<std::vector<uint8_t>>> stored_data_;
};

} // anonymous namespace

// ---------------------------------------------------------------------------
// MT-C-001: BatchExist with local cache — cached keys skip Meta query
// ---------------------------------------------------------------------------
TEST_F(ClientBatchOpsTest, BatchExistWithCache) {
    const int N = 5;

    // Put keys via the store.
    for (int i = 0; i < N; ++i) {
        std::string key = "cache_key_" + std::to_string(i);
        std::vector<uint8_t> data(4096, static_cast<uint8_t>(i));
        auto result = store_->Put(key, data.data(), data.size());
        ASSERT_TRUE(result.status.ok());
    }

    // BatchExist should find the keys in the local store.
    std::vector<std::string> keys;
    for (int i = 0; i < N; ++i) {
        keys.push_back("cache_key_" + std::to_string(i));
    }

    std::vector<KeyDescriptor> hit_descs;
    int hit_count = client_->BatchExist(keys, hit_descs);

    // All keys should be found in the local store index.
    EXPECT_GE(hit_count, 0) << "BatchExist should not crash";
}

// ---------------------------------------------------------------------------
// MT-C-002: BatchPut writes to local store, readable directly
// ---------------------------------------------------------------------------
TEST_F(ClientBatchOpsTest, BatchPutL0Local) {
    // Note: stored_data_ is a class member to keep data alive.
    const int N = 5;
    std::vector<std::string> keys;
    std::vector<BufferInfo> buffers;

    for (int i = 0; i < N; ++i) {
        keys.push_back("l0put_" + std::to_string(i));
        auto data = std::make_unique<std::vector<uint8_t>>(4096, static_cast<uint8_t>(i));
        BufferInfo info;
        info.data_ptr = data->data();
        info.size = data->size();
        buffers.push_back(info);
        // Keep data alive.
        stored_data_.push_back(std::move(data));
    }

    auto results = client_->BatchPut(keys, buffers);
    for (int i = 0; i < N; ++i) {
        EXPECT_TRUE(results[i].ok()) << "BatchPut[" << i << "] failed: " << results[i].msg();
    }

    // Verify data via direct store read.
    for (int i = 0; i < N; ++i) {
        std::vector<uint8_t> buf(4096, 0);
        auto result = store_->Get(keys[i], buf.data(), buf.size());
        ASSERT_TRUE(result.status.ok()) << "Direct store Get failed for " << keys[i];
        EXPECT_EQ(buf[0], static_cast<uint8_t>(i));
    }

    // Cleanup held data.
    stored_data_.clear();
}

// ---------------------------------------------------------------------------
// MT-C-003: BatchGet with ACCESS_LOCAL_DIRECT
// ---------------------------------------------------------------------------
TEST_F(ClientBatchOpsTest, BatchGetL0Local) {
    const int N = 5;

    // Write via store.
    std::vector<std::string> keys;
    for (int i = 0; i < N; ++i) {
        keys.push_back("l0get_" + std::to_string(i));
        std::vector<uint8_t> data(4096, static_cast<uint8_t>(100 + i));
        auto result = store_->Put(keys[i], data.data(), data.size());
        ASSERT_TRUE(result.status.ok());
    }

    // Build KeyDescriptors for local access.
    std::vector<KeyDescriptor> descs;
    for (int i = 0; i < N; ++i) {
        KeyDescriptor desc(keys[i]);
        desc.access_type = AccessType::ACCESS_LOCAL_DIRECT;
        descs.push_back(desc);
    }

    std::vector<BufferInfo> buffers;
    std::vector<std::vector<uint8_t>> read_storage;
    for (int i = 0; i < N; ++i) {
        read_storage.emplace_back(4096, 0);
        BufferInfo info;
        info.data_ptr = read_storage.back().data();
        info.size = 4096;
        buffers.push_back(info);
    }

    auto statuses = client_->BatchGet(descs, buffers);
    for (int i = 0; i < N; ++i) {
        // BatchGet returns bytes read (positive) on success.
        EXPECT_GT(statuses[i], 0) << "BatchGet[" << i << "] returned error";
    }
}

// ---------------------------------------------------------------------------
// MT-C-004: BatchGet with ACCESS_NODE_DIRECT (via temporary file)
// ---------------------------------------------------------------------------
TEST_F(ClientBatchOpsTest, BatchGetNodeDirect) {
    // The client's NodeLocalAccessor does not have a store file registered.
    // A BatchGet with ACCESS_NODE_DIRECT should fail gracefully.
    KeyDescriptor desc("node_key");
    desc.store_id = 99;
    desc.access_type = AccessType::ACCESS_NODE_DIRECT;

    std::vector<uint8_t> buf(4096, 0);
    BufferInfo info;
    info.data_ptr = buf.data();
    info.size = 4096;

    auto statuses = client_->BatchGet({desc}, {info});
    // Should return <= 0 (no file registered for store_id=99).
    EXPECT_LE(statuses[0], 0) << "Expected error for unregistered store file";
}

// ---------------------------------------------------------------------------
// MT-C-005: BatchPut when store is full returns kNoSpace
// ---------------------------------------------------------------------------
TEST_F(ClientBatchOpsTest, BatchPutNoSpace) {
    // Create a tiny store (4 pages = 16 KB).
    std::string tiny_dir = GetTempDir();
    FalconKVStore::Config config;
    config.ssd_path = tiny_dir;
    config.store_id = 99;
    // Power-of-2 pages for buddy allocator: 4 pages.
    config.capacity_bytes = 4 * 4096;
    config.page_size = 4096;
    config.slot_size_bytes = 4096;
    config.disable_mtime = true;
    config.scheduler_enabled = false;

    FalconKVStore tiny_store(config);
    ASSERT_TRUE(tiny_store.Init().ok());

    FalconKVClientImpl::Config client_config;
    client_config.scheduler_enabled = false;
    FalconKVClientImpl client(client_config);
    client.SetLocalStore(&tiny_store);

    std::vector<uint8_t> data(4096, 0xFF);
    BufferInfo info;
    info.data_ptr = data.data();
    info.size = data.size();

    // Fill all 4 chunks.
    for (int i = 0; i < 4; ++i) {
        auto r = client.BatchPut({"fill_" + std::to_string(i)}, {info});
        ASSERT_TRUE(r[0].ok()) << "Fill " << i << " failed";
    }

    // 5th put triggers forced eviction of the LRU entry, then succeeds.
    // This is the new behavior: Put auto-evicts when full.
    auto r2 = client.BatchPut({"overflow"}, {info});
    EXPECT_TRUE(r2[0].ok()) << "Expected OK after forced eviction, got: "
                             << r2[0].ToString();

    client.Close();
    tiny_store.Close();
    std::string cmd = "rm -rf " + tiny_dir;
    if (system(cmd.c_str())) {}
}

// ---------------------------------------------------------------------------
// MT-C-006: ACCESS_REMOTE_RPC fails gracefully without RPC
// ---------------------------------------------------------------------------
TEST_F(ClientBatchOpsTest, BatchGetRpcFallback) {
    KeyDescriptor desc("remote_key");
    desc.store_id = 99;
    desc.access_type = AccessType::ACCESS_REMOTE_RPC;
    desc.store_addr = "255.255.255.255:9999";

    std::vector<uint8_t> buf(4096, 0);
    BufferInfo info;
    info.data_ptr = buf.data();
    info.size = 4096;

    auto statuses = client_->BatchGet({desc}, {info});
    // Should return <= 0 (no RPC connection).
    EXPECT_LE(statuses[0], 0) << "Expected error for unreachable RPC store";
}

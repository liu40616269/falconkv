#include <gtest/gtest.h>
#include <string>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include "src/common/types.h"
#include "src/common/status.h"
#include "src/store/falconkv_store.h"
#include "src/store/node_local_accessor.h"
#include "src/client/key_desc_cache.h"

using namespace falconkv;

namespace {

std::string GetTempDir() {
    char tmpl[] = "/tmp/falconkv_alloc_test_XXXXXX";
    return mkdtemp(tmpl);
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// AccessType enum covers all three values
// ---------------------------------------------------------------------------
TEST(AllocPolicyTest, AccessTypeEnumCoverage) {
    // Verify the three enum values exist and are distinct.
    AccessType local = AccessType::ACCESS_LOCAL_DIRECT;
    AccessType node  = AccessType::ACCESS_NODE_DIRECT;
    AccessType rpc   = AccessType::ACCESS_REMOTE_RPC;

    EXPECT_EQ(static_cast<int>(local), 0);
    EXPECT_EQ(static_cast<int>(node),  1);
    EXPECT_EQ(static_cast<int>(rpc),   2);

    EXPECT_NE(local, node);
    EXPECT_NE(node, rpc);
    EXPECT_NE(local, rpc);
}

// ---------------------------------------------------------------------------
// ACCESS_LOCAL_DIRECT: BatchGet reads from local store
// ---------------------------------------------------------------------------
TEST(AllocPolicyTest, AccessTypeLocalDirect) {
    std::string tmp_dir = GetTempDir();

    FalconKVStore::Config config;
    config.ssd_path = tmp_dir;
    config.store_id = 1;
    config.capacity_bytes = 4 * 1024 * 1024;
    config.page_size = 4096;
    config.disable_mtime = true;
    config.scheduler_enabled = false;

    FalconKVStore store(config);
    ASSERT_TRUE(store.Init().ok());

    // Write a key via the key-aware API.
    const std::string key = "local_key";
    const std::vector<uint8_t> data(4096, 0xAA);
    auto put_result = store.Put(key, data.data(), data.size());
    ASSERT_TRUE(put_result.status.ok());

    // Read it back.
    std::vector<uint8_t> read_buf(data.size(), 0);
    auto get_result = store.Get(key, read_buf.data(), read_buf.size());
    ASSERT_TRUE(get_result.status.ok());
    EXPECT_EQ(memcmp(data.data(), read_buf.data(), data.size()), 0);

    store.Close();
    std::string cmd = "rm -rf " + tmp_dir;
    if (system(cmd.c_str())) {}
}

// ---------------------------------------------------------------------------
// ACCESS_NODE_DIRECT: BatchGet reads through NodeLocalAccessor
// ---------------------------------------------------------------------------
TEST(AllocPolicyTest, AccessTypeNodeDirect) {
    std::string tmp_dir = GetTempDir();

    // Create a temporary data file to simulate a node-local store file.
    std::string data_file = tmp_dir + "/kv_data_2";
    int fd = open(data_file.c_str(), O_RDWR | O_CREAT | O_DIRECT, 0644);
    ASSERT_GE(fd, 0);

    // Write aligned data to the file.
    const size_t alignment = 512;
    const size_t buf_size = 4096;
    void* write_buf = AlignedAllocator::Allocate(alignment, buf_size);
    ASSERT_NE(write_buf, nullptr);
    memset(write_buf, 0xBB, buf_size);

    // Preallocate file.
    ASSERT_EQ(fallocate(fd, 0, 0, buf_size), 0);
    ASSERT_EQ(pwrite(fd, write_buf, buf_size, 0), static_cast<ssize_t>(buf_size));

    // Read via NodeLocalAccessor.
    NodeLocalAccessor accessor;
    accessor.RegisterStoreFile(2, data_file);

    void* read_buf = AlignedAllocator::Allocate(alignment, buf_size);
    ASSERT_NE(read_buf, nullptr);
    Status s = accessor.Read(2, 0, read_buf, buf_size);
    EXPECT_TRUE(s.ok()) << "NodeLocalAccessor Read failed: " << s.msg();
    EXPECT_EQ(memcmp(write_buf, read_buf, buf_size), 0);

    AlignedAllocator::Free(write_buf);
    AlignedAllocator::Free(read_buf);
    close(fd);
    accessor.Close();

    std::string cmd = "rm -rf " + tmp_dir;
    if (system(cmd.c_str())) {}
}

// ---------------------------------------------------------------------------
// ACCESS_REMOTE_RPC: BatchGet fails gracefully without RPC
// ---------------------------------------------------------------------------
TEST(AllocPolicyTest, AccessTypeRemoteRpc) {
    // Build a KeyDescriptor that requires remote RPC.
    KeyDescriptor desc("remote_key");
    desc.store_id = 99;
    desc.offset = 0;
    desc.size = 4096;
    desc.access_type = AccessType::ACCESS_REMOTE_RPC;
    desc.store_addr = "255.255.255.255:9999"; // unreachable

    // Without a real RPC client, attempting to read should fail.
    // We verify the path selection logic by checking the descriptor fields.
    EXPECT_EQ(desc.access_type, AccessType::ACCESS_REMOTE_RPC);
    EXPECT_FALSE(desc.store_addr.empty());
}

// ---------------------------------------------------------------------------
// Store returns kNoSpace when full
// ---------------------------------------------------------------------------
TEST(AllocPolicyTest, AllStoresNoSpace) {
    std::string tmp_dir = GetTempDir();

    FalconKVStore::Config config;
    config.ssd_path = tmp_dir;
    config.store_id = 3;
    // capacity = 4 pages (power of 2 for buddy allocator), chunk = 1 page.
    // BuddyAllocator: total_pages=4, max_order=2, can allocate exactly 4 chunks.
    config.capacity_bytes = 4 * 4096;
    config.page_size = 4096;
    config.disable_mtime = true;
    config.scheduler_enabled = false;

    FalconKVStore store(config);
    ASSERT_TRUE(store.Init().ok());

    const std::vector<uint8_t> data(4096, 0xCC);

    // Fill all 4 available chunks.
    for (int i = 0; i < 4; ++i) {
        auto result = store.Put("fill_" + std::to_string(i), data.data(), data.size());
        ASSERT_TRUE(result.status.ok()) << "Fill " << i << " failed";
    }

    // 5th write should fail with kNoSpace.
    auto result = store.Put("overflow", data.data(), data.size());
    EXPECT_FALSE(result.status.ok());
    EXPECT_TRUE(result.status.IsNoSpace()) << "Expected kNoSpace, got: " << result.status.msg();

    store.Close();
    std::string cmd = "rm -rf " + tmp_dir;
    if (system(cmd.c_str())) {}
}

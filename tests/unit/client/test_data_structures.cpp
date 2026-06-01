#include <gtest/gtest.h>
#include <string>
#include <cstdint>

#include "src/client/key_desc_cache.h"
#include "src/client/falconkv_client_impl.h"

using namespace falconkv;

// ---------------------------------------------------------------------------
// KeyDescriptor access_type enum
// ---------------------------------------------------------------------------
TEST(DataStructures, KeyDescriptorAccessTypeValues) {
    EXPECT_EQ(static_cast<int>(AccessType::ACCESS_LOCAL_DIRECT), 0);
    EXPECT_EQ(static_cast<int>(AccessType::ACCESS_NODE_DIRECT), 1);
    EXPECT_EQ(static_cast<int>(AccessType::ACCESS_REMOTE_RPC), 2);
}

TEST(DataStructures, KeyDescriptorDefaultAccessType) {
    KeyDescriptor desc;
    EXPECT_EQ(desc.access_type, AccessType::ACCESS_REMOTE_RPC);
}

TEST(DataStructures, KeyDescriptorFields) {
    KeyDescriptor desc;
    desc.key = "my_key";
    desc.store_id = 42;
    desc.offset = 1024;
    desc.size = 4096;
    desc.chunk_size = 8192;
    desc.access_time_ms = 12345678;
    desc.data_file = "/data/kv_data_42";
    desc.access_type = AccessType::ACCESS_LOCAL_DIRECT;

    EXPECT_EQ(desc.key, "my_key");
    EXPECT_EQ(desc.store_id, 42u);
    EXPECT_EQ(desc.offset, 1024u);
    EXPECT_EQ(desc.size, 4096u);
    EXPECT_EQ(desc.chunk_size, 8192u);
    EXPECT_EQ(desc.access_time_ms, 12345678u);
    EXPECT_EQ(desc.data_file, "/data/kv_data_42");
    EXPECT_EQ(desc.access_type, AccessType::ACCESS_LOCAL_DIRECT);
}

// ---------------------------------------------------------------------------
// AllocResult fields
// ---------------------------------------------------------------------------
TEST(DataStructures, AllocResultDefaultValues) {
    AllocResult result;
    EXPECT_EQ(result.status, 0);
    EXPECT_EQ(result.store_id, 0u);
    EXPECT_EQ(result.offset, 0u);
    EXPECT_TRUE(result.store_addr.empty());
    EXPECT_TRUE(result.data_file.empty());
    EXPECT_EQ(result.access_type, AccessType::ACCESS_REMOTE_RPC);
}

TEST(DataStructures, AllocResultAccessTypeEnum) {
    EXPECT_EQ(static_cast<int>(AccessType::ACCESS_LOCAL_DIRECT), 0);
    EXPECT_EQ(static_cast<int>(AccessType::ACCESS_NODE_DIRECT), 1);
    EXPECT_EQ(static_cast<int>(AccessType::ACCESS_REMOTE_RPC), 2);
}

TEST(DataStructures, AllocResultFieldsAssignment) {
    AllocResult result;
    result.status = 1;
    result.store_id = 5;
    result.offset = 999;
    result.store_addr = "10.0.0.1:8901";
    result.data_file = "/data/kv_data_5";
    result.access_type = AccessType::ACCESS_NODE_DIRECT;

    EXPECT_EQ(result.status, 1);
    EXPECT_EQ(result.store_id, 5u);
    EXPECT_EQ(result.offset, 999u);
    EXPECT_EQ(result.store_addr, "10.0.0.1:8901");
    EXPECT_EQ(result.data_file, "/data/kv_data_5");
    EXPECT_EQ(result.access_type, AccessType::ACCESS_NODE_DIRECT);
}

// ---------------------------------------------------------------------------
// BufferInfo struct
// ---------------------------------------------------------------------------
TEST(DataStructures, BufferInfoFields) {
    int value = 42;
    BufferInfo info;
    info.data_ptr = &value;
    info.size = sizeof(value);

    EXPECT_EQ(info.data_ptr, &value);
    EXPECT_EQ(info.size, sizeof(value));

    EXPECT_NE(info.data_ptr, nullptr);
    EXPECT_EQ(*static_cast<int*>(info.data_ptr), 42);
}

TEST(DataStructures, BufferInfoDefaultInit) {
    BufferInfo info;
    // data_ptr is uninitialized by default (no constructor), but we can
    // verify the struct is usable after assignment.
    uint8_t buf[16];
    info.data_ptr = buf;
    info.size = 16;
    EXPECT_EQ(info.data_ptr, buf);
    EXPECT_EQ(info.size, 16u);
}

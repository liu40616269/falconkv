#include <gtest/gtest.h>
#include <string>
#include <sys/stat.h>
#include <cstdlib>

#include "src/store/falconkv_store.h"

using namespace falconkv;

// ---------------------------------------------------------------------------
// File path includes store_id
// ---------------------------------------------------------------------------
TEST(FileNaming, FilePathIncludesStoreId) {
    FalconKVStore::Config config;
    config.ssd_path = "/data/falconkv";
    config.store_id = 42;

    FalconKVStore store(config);

    EXPECT_NE(store.data_file().find("42"), std::string::npos)
        << "Data file path should contain store_id 42, got: " << store.data_file();
    EXPECT_EQ(store.data_file(), "/data/falconkv/kv_data_42");
}

TEST(FileNaming, StoreIdZero) {
    FalconKVStore::Config config;
    config.ssd_path = "/tmp/falconkv_test";
    config.store_id = 0;

    FalconKVStore store(config);

    EXPECT_NE(store.data_file().find("0"), std::string::npos)
        << "Data file path should contain store_id 0, got: " << store.data_file();
    EXPECT_EQ(store.data_file(), "/tmp/falconkv_test/kv_data_0");
}

// ---------------------------------------------------------------------------
// Different store_ids produce different paths
// ---------------------------------------------------------------------------
TEST(FileNaming, DifferentStoreIdsProduceDifferentPaths) {
    FalconKVStore::Config config1;
    config1.ssd_path = "/data/falconkv";
    config1.store_id = 1;

    FalconKVStore::Config config2;
    config2.ssd_path = "/data/falconkv";
    config2.store_id = 2;

    FalconKVStore store1(config1);
    FalconKVStore store2(config2);

    EXPECT_NE(store1.data_file(), store2.data_file());
}

TEST(FileNaming, StoreIdAccessor) {
    FalconKVStore::Config config;
    config.store_id = 99;

    FalconKVStore store(config);
    EXPECT_EQ(store.store_id(), 99u);
}

TEST(FileNaming, PathFormat) {
    FalconKVStore::Config config;
    config.ssd_path = "/mnt/ssd";
    config.store_id = 7;

    FalconKVStore store(config);

    // Verify the naming pattern: <ssd_path>/kv_data_<store_id>
    EXPECT_EQ(store.data_file(), "/mnt/ssd/kv_data_7");
}

// ---------------------------------------------------------------------------
// Init() creates a data file with size >= capacity_bytes
// ---------------------------------------------------------------------------
TEST(FileNaming, FallocatePreallocation) {
    std::string tmp_dir = "/tmp/falconkv_test_fallocate_" + std::to_string(getpid());
    mkdir(tmp_dir.c_str(), 0755);

    FalconKVStore::Config config;
    config.ssd_path = tmp_dir;
    config.store_id = 42;
    config.capacity_bytes = 4 * 1024 * 1024; // 4 MB
    config.page_size = 4096;
    config.disable_mtime = true;
    config.scheduler_enabled = false;

    FalconKVStore store(config);
    Status s = store.Init();
    ASSERT_TRUE(s.ok()) << "Init failed: " << s.msg();

    // Check file size via stat().
    struct stat st;
    std::string data_file = store.data_file();
    ASSERT_EQ(stat(data_file.c_str(), &st), 0)
        << "stat() failed for " << data_file;
    EXPECT_GE(static_cast<uint64_t>(st.st_size), config.capacity_bytes)
        << "File size " << st.st_size << " is less than capacity_bytes " << config.capacity_bytes;

    store.Close();
    std::string cmd = "rm -rf " + tmp_dir;
    if (system(cmd.c_str())) {}
}

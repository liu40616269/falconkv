#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <chrono>

#include "src/store/store_server.h"
#include "src/store/store_rpc_client.h"

namespace falconkv {
namespace {

class StoreRpcClientTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir_ = std::filesystem::temp_directory_path() / "falconkv_test_rpc";
        std::filesystem::create_directories(test_dir_);

        FalconKVStore::Config config;
        config.ssd_path = test_dir_.string();
        config.store_id = 1;
        config.capacity_bytes = 16 * 1024 * 1024; // 16MB for testing
        config.page_size = 512;

        listen_addr_ = "127.0.0.1:18901";
        server_ = std::make_unique<StoreServer>(config, listen_addr_);

        Status s = server_->Start();
        ASSERT_TRUE(s.ok()) << s.msg();
    }

    void TearDown() override {
        server_->Stop();
        server_.reset();
        std::filesystem::remove_all(test_dir_);
    }

    std::filesystem::path test_dir_;
    std::unique_ptr<StoreServer> server_;
    std::string listen_addr_;
};

TEST_F(StoreRpcClientTest, ConnectAndPing) {
    StoreRpcClient client;
    Status s = client.Connect(listen_addr_);
    ASSERT_TRUE(s.ok()) << s.msg();

    s = client.Ping();
    EXPECT_TRUE(s.ok()) << s.msg();
}

TEST_F(StoreRpcClientTest, ReadOffsetBased) {
    StoreRpcClient client;
    ASSERT_TRUE(client.Connect(listen_addr_).ok());

    // Write data directly to store offset 0 (page-aligned)
    // The store's Write() is used internally; we test Read RPC path
    std::string write_data(512, 'Z');
    Status ws = server_->GetStore()->Write(0, write_data.data(), write_data.size());
    ASSERT_TRUE(ws.ok()) << ws.msg();

    // Read back via RPC
    std::string read_buf(write_data.size(), '\0');
    Status rs = client.Read(0, read_buf.data(), read_buf.size());
    ASSERT_TRUE(rs.ok()) << rs.msg();

    EXPECT_EQ(write_data, read_buf);
}

TEST_F(StoreRpcClientTest, ConnectToInvalidAddr) {
    StoreRpcClient client;
    Status s = client.Connect("256.256.256.256:9999");
    EXPECT_FALSE(s.ok());
}

} // namespace
} // namespace falconkv

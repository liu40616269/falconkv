#include <gtest/gtest.h>

#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "src/store/node_local_accessor.h"

namespace falconkv {
namespace {

class NodeLocalAccessorTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir_ = std::filesystem::temp_directory_path() / "falconkv_test_nla";
        std::filesystem::create_directories(test_dir_);
        data_file_ = test_dir_ / "kv_data_42";

        // Create and preallocate a data file
        std::ofstream ofs(data_file_, std::ios::binary | std::ios::trunc);
        // Write 4MB of zeros
        std::vector<char> zeros(4 * 1024 * 1024, 0);
        ofs.write(zeros.data(), zeros.size());
        ofs.close();

        accessor_.RegisterStoreFile(42, data_file_.string());
    }

    void TearDown() override {
        accessor_.Close();
        std::filesystem::remove_all(test_dir_);
    }

    std::filesystem::path test_dir_;
    std::filesystem::path data_file_;
    NodeLocalAccessor accessor_;
};

TEST_F(NodeLocalAccessorTest, WriteAndReadAligned) {
    const std::string test_data(4096, 'A');
    uint64_t offset = 0;

    Status ws = accessor_.Write(42, offset, test_data.data(), test_data.size());
    ASSERT_TRUE(ws.ok()) << ws.msg();

    std::string read_buf(test_data.size(), '\0');
    Status rs = accessor_.Read(42, offset, read_buf.data(), read_buf.size());
    ASSERT_TRUE(rs.ok()) << rs.msg();

    EXPECT_EQ(test_data, read_buf);
}

TEST_F(NodeLocalAccessorTest, WriteAndReadUnaligned) {
    // Use a non-aligned size and data
    const std::string test_data = "Hello, FalconKV!";
    uint64_t offset = 4096;

    Status ws = accessor_.Write(42, offset, test_data.data(),
                                 static_cast<uint32_t>(test_data.size()));
    ASSERT_TRUE(ws.ok()) << ws.msg();

    std::string read_buf(test_data.size(), '\0');
    Status rs = accessor_.Read(42, offset, read_buf.data(),
                                static_cast<uint32_t>(read_buf.size()));
    ASSERT_TRUE(rs.ok()) << rs.msg();

    EXPECT_EQ(test_data, read_buf);
}

TEST_F(NodeLocalAccessorTest, WriteAndReadMultipleOffsets) {
    const std::string data1(4096, 'X');
    const std::string data2(4096, 'Y');

    ASSERT_TRUE(accessor_.Write(42, 0, data1.data(), data1.size()).ok());
    ASSERT_TRUE(accessor_.Write(42, 4096, data2.data(), data2.size()).ok());

    std::string buf1(data1.size(), '\0');
    std::string buf2(data2.size(), '\0');
    ASSERT_TRUE(accessor_.Read(42, 0, buf1.data(), buf1.size()).ok());
    ASSERT_TRUE(accessor_.Read(42, 4096, buf2.data(), buf2.size()).ok());

    EXPECT_EQ(data1, buf1);
    EXPECT_EQ(data2, buf2);
}

TEST_F(NodeLocalAccessorTest, ReadUnknownStoreId) {
    char buf[64];
    Status rs = accessor_.Read(999, 0, buf, sizeof(buf));
    EXPECT_FALSE(rs.ok());
    EXPECT_TRUE(rs.IsIOError());
}

} // namespace
} // namespace falconkv

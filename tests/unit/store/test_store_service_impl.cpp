#include <gtest/gtest.h>

#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "src/store/falconkv_store.h"
#include "src/store/store_service_impl.h"

namespace falconkv {
namespace {

class StoreServiceImplTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir_ = std::filesystem::temp_directory_path() / "falconkv_test_svc";
        std::filesystem::create_directories(test_dir_);

        FalconKVStore::Config config;
        config.ssd_path = test_dir_.string();
        config.store_id = 1;
        config.capacity_bytes = 16 * 1024 * 1024; // 16MB for testing
        config.page_size = 512;

        store_ = std::make_unique<FalconKVStore>(config);
        Status s = store_->Init();
        ASSERT_TRUE(s.ok()) << s.msg();

        service_ = std::make_unique<StoreServiceImpl>(store_.get());
    }

    void TearDown() override {
        service_.reset();
        store_->Close();
        store_.reset();
        std::filesystem::remove_all(test_dir_);
    }

    std::filesystem::path test_dir_;
    std::unique_ptr<FalconKVStore> store_;
    std::unique_ptr<StoreServiceImpl> service_;
};

// A trivial controller/closure pair for testing without brpc::Controller
class TestController : public ::google::protobuf::RpcController {
public:
    void Reset() override {}
    bool Failed() const override { return false; }
    std::string ErrorText() const override { return ""; }
    void StartCancel() override {}
    void SetFailed(const std::string&) override {}
    bool IsCanceled() const override { return false; }
    void NotifyOnCancel(::google::protobuf::Closure*) override {}
};

class TestDone : public ::google::protobuf::Closure {
public:
    void Run() override { called_ = true; }
    bool called() const { return called_; }
private:
    bool called_ = false;
};

TEST_F(StoreServiceImplTest, PutThenReadByKey) {
    // Put data via Store key-aware API
    std::string data = "hello store";
    auto put_result = store_->Put("key1", data.data(), data.size());
    ASSERT_TRUE(put_result.status.ok());

    // Read back via GetByKey RPC
    GetByKeyRequest req;
    req.set_key("key1");

    GetByKeyResponse resp;
    TestController cntl;
    TestDone done;
    service_->GetByKey(&cntl, &req, &resp, &done);
    EXPECT_EQ(0, resp.status());
    EXPECT_EQ(data, resp.data());
    EXPECT_TRUE(done.called());
}

TEST_F(StoreServiceImplTest, Ping) {
    PingRequest ping_req;
    ping_req.set_timestamp_ns(12345);

    PongResponse pong_resp;
    TestController cntl;
    TestDone done;

    service_->Ping(&cntl, &ping_req, &pong_resp, &done);
    EXPECT_EQ(0, pong_resp.status());
    EXPECT_EQ(12345u, pong_resp.timestamp_ns());
}

TEST_F(StoreServiceImplTest, ReadOffsetBased) {
    // Write data directly using offset-based API
    std::string data = "offset data";
    Status s = store_->Write(0, data.data(), data.size());
    ASSERT_TRUE(s.ok());

    // Read back via Read RPC
    ReadRequest req;
    req.set_offset(0);
    req.set_size(data.size());

    ReadResponse resp;
    TestController cntl;
    TestDone done;
    service_->Read(&cntl, &req, &resp, &done);
    EXPECT_EQ(0, resp.status());
    EXPECT_EQ(data, resp.data());
}

TEST_F(StoreServiceImplTest, BatchGetByKey) {
    // Put multiple keys
    std::string data1 = "aaaa";
    std::string data2 = "bbbb";
    store_->Put("key1", data1.data(), data1.size());
    store_->Put("key2", data2.data(), data2.size());

    // Batch read via RPC
    BatchGetByKeyRequest req;
    req.add_keys("key1");
    req.add_keys("key2");
    req.add_keys("key3"); // nonexistent

    BatchGetByKeyResponse resp;
    TestController cntl;
    TestDone done;
    service_->BatchGetByKey(&cntl, &req, &resp, &done);
    EXPECT_EQ(1, resp.status()); // not all ok due to key3

    ASSERT_EQ(3, resp.statuses_size());
    EXPECT_EQ(0, resp.statuses(0));
    EXPECT_EQ(0, resp.statuses(1));
    EXPECT_EQ(static_cast<int>(Status::kNotFound), resp.statuses(2));

    EXPECT_EQ(data1, resp.data_segments(0));
    EXPECT_EQ(data2, resp.data_segments(1));
}

} // namespace
} // namespace falconkv

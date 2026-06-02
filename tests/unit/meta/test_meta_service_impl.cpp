#include <gtest/gtest.h>

#include "src/meta/meta_manager.h"
#include "src/meta/meta_service_impl.h"
#include "falconkv_meta.pb.h"

namespace falconkv {

// A trivial "controller" that satisfies the RpcController interface
// for in-process (non-RPC) testing.
class TestController : public ::google::protobuf::RpcController {
public:
    void Reset() override {}
    bool Failed() const override { return failed_; }
    std::string ErrorText() const override { return error_text_; }
    void StartCancel() override {}
    void SetFailed(const std::string& reason) override {
        failed_ = true;
        error_text_ = reason;
    }
    bool IsCanceled() const override { return false; }
    void NotifyOnCancel(google::protobuf::Closure*) override {}

private:
    bool failed_ = false;
    std::string error_text_;
};

// A simple closure that sets a flag when run.
class TestClosure : public ::google::protobuf::Closure {
public:
    explicit TestClosure(bool* flag) : flag_(flag) {}
    void Run() override { *flag_ = true; }

private:
    bool* flag_;
};

class MetaServiceImplTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Register a store.
        StoreInfo info;
        info.store_id = 1;
        info.node_id = 0;
        info.store_addr = "localhost:8901";
        info.data_file = "/tmp/falconkv_test/data_0.db";

        ASSERT_TRUE(meta_.RegisterStore(info).ok());

        service_ = std::make_unique<MetaServiceImpl>(&meta_);
    }

    MetaManager meta_;
    std::unique_ptr<MetaServiceImpl> service_;
};

TEST_F(MetaServiceImplTest, StoreRegisterSucceeds) {
    StoreRegisterRequest req;
    req.set_store_id(2);
    req.set_node_host("localhost");
    req.set_node_port(8902);
    req.set_data_file("/tmp/data_2.db");

    StoreRegisterResponse resp;
    TestController cntl;
    bool done_called = false;
    TestClosure done(&done_called);

    service_->StoreRegister(&cntl, &req, &resp, &done);

    EXPECT_TRUE(done_called);
    EXPECT_EQ(resp.status(), 0);
}

TEST_F(MetaServiceImplTest, SyncCommitAndBatchExist) {
    // 1) SyncCommit some keys
    SyncCommitRequest commit_req;
    commit_req.set_store_id(1);

    auto* desc1 = commit_req.add_key_records();
    desc1->set_key("key1");
    desc1->set_offset(100);
    desc1->set_size(50);

    auto* desc2 = commit_req.add_key_records();
    desc2->set_key("key2");
    desc2->set_offset(200);
    desc2->set_size(75);

    SyncCommitResponse commit_resp;
    TestController cntl;
    bool done = false;
    TestClosure closure(&done);

    service_->SyncCommit(&cntl, &commit_req, &commit_resp, &closure);
    EXPECT_TRUE(done);
    EXPECT_EQ(commit_resp.status(), 0);

    // 2) BatchExist
    BatchExistRequest exist_req;
    exist_req.add_keys("key1");
    exist_req.add_keys("key2");
    exist_req.add_keys("key3"); // doesn't exist

    BatchExistResponse exist_resp;
    done = false;
    service_->BatchExist(&cntl, &exist_req, &exist_resp, &closure);
    EXPECT_TRUE(done);
    EXPECT_EQ(exist_resp.hit_count(), 2);
    EXPECT_EQ(exist_resp.key_descs_size(), 3);
}

TEST_F(MetaServiceImplTest, BatchLookup) {
    // SyncCommit a key
    SyncCommitRequest commit_req;
    commit_req.set_store_id(1);
    auto* desc = commit_req.add_key_records();
    desc->set_key("lookup_key");
    desc->set_offset(300);
    desc->set_size(100);

    SyncCommitResponse commit_resp;
    TestController cntl;
    bool done = false;
    TestClosure closure(&done);
    service_->SyncCommit(&cntl, &commit_req, &commit_resp, &closure);

    // BatchLookup
    BatchLookupRequest lookup_req;
    lookup_req.add_keys("lookup_key");
    lookup_req.add_keys("nonexistent");

    BatchLookupResponse lookup_resp;
    done = false;
    service_->BatchLookup(&cntl, &lookup_req, &lookup_resp, &closure);
    EXPECT_TRUE(done);

    // Should have 1 entry for the found key
    int found = 0;
    for (int i = 0; i < lookup_resp.key_descs_size(); ++i) {
        if (!lookup_resp.key_descs(i).key().empty()) {
            ++found;
            EXPECT_EQ(lookup_resp.key_descs(i).key(), "lookup_key");
        }
    }
    EXPECT_EQ(found, 1);
}

TEST_F(MetaServiceImplTest, SyncRemove) {
    // SyncCommit a key
    SyncCommitRequest commit_req;
    commit_req.set_store_id(1);
    auto* desc = commit_req.add_key_records();
    desc->set_key("remove_key");
    desc->set_offset(400);
    desc->set_size(100);

    SyncCommitResponse commit_resp;
    TestController cntl;
    bool done = false;
    TestClosure closure(&done);
    service_->SyncCommit(&cntl, &commit_req, &commit_resp, &closure);

    // SyncRemove
    SyncRemoveRequest remove_req;
    remove_req.set_store_id(1);
    remove_req.add_keys("remove_key");

    SyncRemoveResponse remove_resp;
    done = false;
    service_->SyncRemove(&cntl, &remove_req, &remove_resp, &closure);
    EXPECT_TRUE(done);
    EXPECT_EQ(remove_resp.status(), 0);

    // Verify key no longer exists via BatchExist
    BatchExistRequest exist_req;
    exist_req.add_keys("remove_key");

    BatchExistResponse exist_resp;
    done = false;
    service_->BatchExist(&cntl, &exist_req, &exist_resp, &closure);
    EXPECT_EQ(exist_resp.hit_count(), 0);
}

} // namespace falconkv

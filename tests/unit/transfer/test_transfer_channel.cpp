#include <gtest/gtest.h>
#include <string>
#include <cstdint>
#include <memory>

#include "src/transfer/transfer_channel.h"

using namespace falconkv;

// ---------------------------------------------------------------------------
// TransferChannel is abstract (cannot instantiate directly)
// ---------------------------------------------------------------------------
TEST(TransferChannel, IsAbstractClass) {
    // TransferChannel has pure virtual methods, so we cannot instantiate it.
    // We verify the abstract nature by attempting to use a concrete subclass.
    // The test below compiles because we only use pointers/references.

    class MockTransferChannel : public TransferChannel {
    public:
        Status Connect(const std::string& addr) override {
            return Status::OK();
        }
        void Disconnect() override {}
        bool IsConnected() const override { return true; }
        Status SyncCall(const std::string& method,
                        const std::string& request_data,
                        std::string* response_data) override {
            return Status::OK();
        }
        Status AsyncCall(const std::string& method,
                         const std::string& request_data,
                         RpcCallback* callback) override {
            return Status::OK();
        }
        Status WriteData(const std::string& addr, uint64_t offset,
                         const void* data, uint32_t size) override {
            return Status::OK();
        }
        Status ReadData(const std::string& addr, uint64_t offset,
                        void* buffer, uint32_t size) override {
            return Status::OK();
        }
        Status BatchWriteData(const std::string& addr,
                              const std::vector<IoSegment>& segments) override {
            return Status::OK();
        }
        Status BatchReadData(const std::string& addr,
                             const std::vector<IoSegment>& segments) override {
            return Status::OK();
        }
    };

    // Can create and use through base pointer.
    std::unique_ptr<TransferChannel> ch = std::make_unique<MockTransferChannel>();
    EXPECT_TRUE(ch->IsConnected());

    Status s = ch->Connect("localhost:8900");
    EXPECT_TRUE(s.ok());

    // Disconnect should not crash.
    ch->Disconnect();
}

// ---------------------------------------------------------------------------
// IoSegment struct fields
// ---------------------------------------------------------------------------
TEST(TransferChannel, IoSegmentFields) {
    IoSegment seg;
    uint8_t buf[16];
    uint8_t data[16] = {0};

    seg.offset = 1024;
    seg.data = data;
    seg.buffer = buf;
    seg.size = 16;

    EXPECT_EQ(seg.offset, 1024u);
    EXPECT_EQ(seg.data, data);
    EXPECT_EQ(seg.buffer, buf);
    EXPECT_EQ(seg.size, 16u);
}

TEST(TransferChannel, IoSegmentDefaultValues) {
    IoSegment seg;
    // offset and size are value-initialized.
    EXPECT_EQ(seg.offset, 0u);
    EXPECT_EQ(seg.size, 0u);
    // data and buffer are pointer-initialized (unspecified in default init).
}

// ---------------------------------------------------------------------------
// RpcCallback interface
// ---------------------------------------------------------------------------
TEST(TransferChannel, RpcCallbackIsAbstract) {
    // RpcCallback has a pure virtual OnComplete, so it is abstract.
    // Verify we can implement it.
    class MockCallback : public RpcCallback {
    public:
        bool called = false;
        Status last_status;
        void OnComplete(Status status) override {
            called = true;
            last_status = status;
        }
    };

    MockCallback cb;
    EXPECT_FALSE(cb.called);

    cb.OnComplete(Status::OK());
    EXPECT_TRUE(cb.called);
    EXPECT_TRUE(cb.last_status.ok());
}

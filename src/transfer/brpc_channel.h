#pragma once
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include "src/transfer/transfer_channel.h"

namespace falconkv {

class BrpcChannel : public TransferChannel {
public:
    BrpcChannel();
    ~BrpcChannel() override;

    Status Connect(const std::string& addr) override;
    void Disconnect() override;
    bool IsConnected() const override;

    Status SyncCall(const std::string& method,
                    const std::string& request_data,
                    std::string* response_data) override;
    Status AsyncCall(const std::string& method,
                     const std::string& request_data,
                     RpcCallback* callback) override;

    Status WriteData(const std::string& addr, uint64_t offset,
                     const void* data, uint32_t size) override;
    Status ReadData(const std::string& addr, uint64_t offset,
                    void* buffer, uint32_t size) override;
    Status BatchWriteData(const std::string& addr,
                          const std::vector<IoSegment>& segments) override;
    Status BatchReadData(const std::string& addr,
                         const std::vector<IoSegment>& segments) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace falconkv

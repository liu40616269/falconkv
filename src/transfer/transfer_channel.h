#pragma once
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <cstdint>
#include "src/common/status.h"

namespace falconkv {

struct IoSegment {
    uint64_t offset = 0;
    const void* data = nullptr;    // for write
    void* buffer = nullptr;        // for read
    uint32_t size = 0;
};

class RpcCallback {
public:
    virtual ~RpcCallback() = default;
    virtual void OnComplete(Status status) = 0;
};

class TransferChannel {
public:
    virtual ~TransferChannel() = default;
    virtual Status Connect(const std::string& addr) = 0;
    virtual void Disconnect() = 0;
    virtual bool IsConnected() const = 0;
    virtual Status SyncCall(const std::string& method,
                            const std::string& request_data,
                            std::string* response_data) = 0;
    virtual Status AsyncCall(const std::string& method,
                             const std::string& request_data,
                             RpcCallback* callback) = 0;
    virtual Status WriteData(const std::string& addr, uint64_t offset,
                             const void* data, uint32_t size) = 0;
    virtual Status ReadData(const std::string& addr, uint64_t offset,
                            void* buffer, uint32_t size) = 0;
    virtual Status BatchWriteData(const std::string& addr,
                                  const std::vector<IoSegment>& segments) = 0;
    virtual Status BatchReadData(const std::string& addr,
                                 const std::vector<IoSegment>& segments) = 0;
};

} // namespace falconkv

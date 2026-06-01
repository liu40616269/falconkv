#pragma once
#include <string>

namespace falconkv {

class Status {
public:
    enum Code {
        kOk = 0,
        kIOError = 1,
        kNotFound = 2,
        kCorruption = 3,
        kRpcError = 4,
        kTimeout = 5,
        kNoSpace = 6,
        kInvalidArg = 7,
        kMaxRetryExceeded = 8,
        kNotSupported = 9,
        kUnknown = 10
    };

    Status() : code_(kOk) {}
    Status(Code code, const std::string& msg) : code_(code), msg_(msg) {}

    static Status OK() { return {}; }
    static Status IoError(const std::string& msg) { return {kIOError, msg}; }
    static Status NotFound(const std::string& msg) { return {kNotFound, msg}; }
    static Status Corruption(const std::string& msg) { return {kCorruption, msg}; }
    static Status RpcError(const std::string& msg) { return {kRpcError, msg}; }
    static Status Timeout(const std::string& msg) { return {kTimeout, msg}; }
    static Status NoSpace(const std::string& msg) { return {kNoSpace, msg}; }
    static Status InvalidArg(const std::string& msg) { return {kInvalidArg, msg}; }
    static Status MaxRetryExceeded() { return {kMaxRetryExceeded, "max retry exceeded"}; }
    static Status NotSupported(const std::string& msg) { return {kNotSupported, msg}; }

    bool ok() const { return code_ == kOk; }
    bool IsIOError() const { return code_ == kIOError; }
    bool IsNotFound() const { return code_ == kNotFound; }
    bool IsRpcError() const { return code_ == kRpcError; }
    bool IsTimeout() const { return code_ == kTimeout; }
    bool IsNetworkError() const { return code_ == kRpcError || code_ == kTimeout; }
    bool IsNoSpace() const { return code_ == kNoSpace; }

    Code code() const { return code_; }
    const std::string& msg() const { return msg_; }
    std::string ToString() const;

private:
    Code code_;
    std::string msg_;
};

} // namespace falconkv

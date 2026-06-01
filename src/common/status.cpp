#include "common/status.h"

namespace falconkv {

std::string Status::ToString() const {
    const char* code_name = "Unknown";
    switch (code_) {
        case kOk:               code_name = "OK"; break;
        case kIOError:          code_name = "IOError"; break;
        case kNotFound:         code_name = "NotFound"; break;
        case kCorruption:       code_name = "Corruption"; break;
        case kRpcError:         code_name = "RpcError"; break;
        case kTimeout:          code_name = "Timeout"; break;
        case kNoSpace:          code_name = "NoSpace"; break;
        case kInvalidArg:       code_name = "InvalidArg"; break;
        case kMaxRetryExceeded: code_name = "MaxRetryExceeded"; break;
        case kNotSupported:     code_name = "NotSupported"; break;
        case kUnknown:          code_name = "Unknown"; break;
    }
    std::string result = std::string("Status(") + code_name + ")";
    if (!msg_.empty()) {
        result += ": " + msg_;
    }
    return result;
}

} // namespace falconkv

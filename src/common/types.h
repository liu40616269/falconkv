#pragma once
#include <string>
#include <cstdint>

namespace falconkv {

// Unified access type enum for all modules.
// Matches the protobuf AccessType enum values.
enum class AccessType {
    ACCESS_LOCAL_DIRECT = 0,
    ACCESS_NODE_DIRECT = 1,
    ACCESS_REMOTE_RPC = 2,
};

struct AllocResult {
    int32_t status = 0;         // 0 = success, <0 = error
    uint32_t store_id = 0;
    uint64_t offset = 0;
    std::string store_addr;
    std::string data_file;
    AccessType access_type = AccessType::ACCESS_REMOTE_RPC;
};

} // namespace falconkv

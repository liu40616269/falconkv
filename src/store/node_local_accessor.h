#pragma once

#include <string>
#include <unordered_map>
#include <mutex>
#include <memory>

#include "src/common/status.h"
#include "src/store/fd_cache.h"

namespace falconkv {

/// NodeLocalAccessor provides DirectIO read/write for data files belonging
/// to any Store on the same node.  It is used by FalconKVClientImpl for
/// ACCESS_NODE_DIRECT operations — the Client calls
/// node_accessor_.Write(store_id, ...) / Read(store_id, ...) without
/// knowing the actual file path.
class NodeLocalAccessor {
public:
    NodeLocalAccessor();
    ~NodeLocalAccessor();

    /// Register a mapping from store_id to the data file path.
    /// Called when the Client learns about a new store (from AllocResult).
    void RegisterStoreFile(uint32_t store_id, const std::string& data_file);

    /// Write data to the file belonging to the given store_id.
    /// Handles DirectIO alignment internally.
    Status Write(uint32_t store_id, uint64_t offset,
                 const void* data, uint32_t size);

    /// Read data from the file belonging to the given store_id.
    /// Handles DirectIO alignment internally.
    Status Read(uint32_t store_id, uint64_t offset,
                void* buffer, uint32_t size);

    /// Close all cached file descriptors.
    void Close();

private:
    int GetFd(uint32_t store_id);

    std::mutex mutex_;
    std::unordered_map<uint32_t, std::string> store_files_;
    std::unique_ptr<FdCache> fd_cache_;
};

} // namespace falconkv

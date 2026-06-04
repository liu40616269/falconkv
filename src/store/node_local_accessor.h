#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <memory>

#include "src/common/status.h"
#include "src/store/fd_cache.h"

namespace falconkv {

class IOThreadPool;
class IOUringEngine;

/// Request structure for batch reads across multiple stores.
struct NodeLocalReadRequest {
    uint32_t store_id;
    uint64_t offset;
    void* buffer;
    uint32_t size;
};

/// NodeLocalAccessor provides DirectIO read/write for data files belonging
/// to any Store on the same node.  It is used by FalconKVClientImpl for
/// ACCESS_NODE_DIRECT operations — the Client calls
/// node_accessor_.Write(store_id, ...) / Read(store_id, ...) without
/// knowing the actual file path.
class NodeLocalAccessor {
public:
    struct Config {
        uint32_t page_size = 4096;
        uint32_t io_threads = 4;
        bool io_uring_enabled = true;
        uint32_t io_uring_queue_depth = 128;
    };

    NodeLocalAccessor();
    ~NodeLocalAccessor();

    /// Initialize IO engines (thread pool + optional io_uring).
    /// Must be called before BatchRead for parallel IO to take effect.
    void InitIOEngines(const Config& config);

    /// Register a mapping from store_id to the data file path.
    /// Called when the Client learns about a new store (from AllocResult).
    /// Returns true if this was a new registration or the path changed,
    /// false if the same mapping already existed (deduplicated).
    bool RegisterStoreFile(uint32_t store_id, const std::string& data_file);

    /// Write data to the file belonging to the given store_id.
    /// Handles DirectIO alignment internally.
    Status Write(uint32_t store_id, uint64_t offset,
                 const void* data, uint32_t size);

    /// Read data from the file belonging to the given store_id.
    /// Handles DirectIO alignment internally.
    Status Read(uint32_t store_id, uint64_t offset,
                void* buffer, uint32_t size);

    /// Batch read from multiple stores. Returns per-request Status.
    /// Uses io_uring if available, falls back to thread pool.
    std::vector<Status> BatchRead(const std::vector<NodeLocalReadRequest>& requests);

    /// Close all cached file descriptors and IO engines.
    void Close();

private:
    int GetDirectFd(uint32_t store_id);
    int GetBufferedFd(uint32_t store_id);
    std::string GetStoreFilePath(uint32_t store_id);

    std::mutex mutex_;
    std::unordered_map<uint32_t, std::string> store_files_;
    std::unique_ptr<FdCache> fd_cache_;

    // IO engines for batch operations
    std::unique_ptr<IOUringEngine> io_uring_engine_;
    std::unique_ptr<IOThreadPool> io_pool_;
    bool io_uring_enabled_ = false;
    uint32_t page_size_ = 4096;
};

} // namespace falconkv

#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <cstdint>
#include <functional>

namespace falconkv {

class BuddyAllocator;

/// PendingEvictQueue holds evicted key records for a grace period before
/// reclaiming their SSD space via BuddyAllocator::Free().  The Meta
/// server has already been notified of the eviction; the grace period
/// prevents in-flight reads from other nodes from hitting freed space.
class PendingEvictQueue {
public:
    struct EvictEntry {
        std::string key;
        uint64_t offset;       ///< SSD offset to free
        uint32_t alloc_size;   ///< actual allocated size for Free()
        uint64_t enqueue_time_ms;  ///< wall-clock time when enqueued
    };

    /// @param grace_period_ms  Grace period in milliseconds (default 5000).
    /// @param allocator        Borrowed pointer — must outlive this object.
    PendingEvictQueue(uint64_t grace_period_ms, BuddyAllocator* allocator);
    ~PendingEvictQueue();

    PendingEvictQueue(const PendingEvictQueue&) = delete;
    PendingEvictQueue& operator=(const PendingEvictQueue&) = delete;

    /// Enqueue a key/offset/alloc_size for deferred space reclamation.
    void Enqueue(const std::string& key, uint64_t offset, uint32_t alloc_size);

    /// Start the background evict loop thread.
    void Start();

    /// Stop the background thread and immediately reclaim all pending entries.
    void Stop();

    /// Return the number of entries currently waiting.
    size_t Size() const;

private:
    void EvictLoop();
    void FlushAll();

    uint64_t grace_period_ms_;
    BuddyAllocator* allocator_;  // not owned

    mutable std::mutex mutex_;
    std::vector<EvictEntry> entries_;

    std::thread thread_;
    std::atomic<bool> running_{false};
};

} // namespace falconkv

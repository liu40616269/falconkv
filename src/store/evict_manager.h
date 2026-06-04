#pragma once

#include <thread>
#include <atomic>
#include <cstdint>
#include <vector>

namespace falconkv {

struct StoreKeyRecord;
class StoreMetaIndex;
class MetaSyncClient;
class PendingEvictQueue;
class SlotAllocator;

/// EvictManager runs a background thread that periodically checks SSD usage.
/// When usage exceeds the high watermark, it selects LRU entries from
/// StoreMetaIndex, notifies Meta via SyncRemove, removes them from the local
/// index, and enqueues them in PendingEvictQueue for deferred space reclamation.
///
/// Additionally provides ForceEvict() for synchronous eviction when Put
/// operations run out of space.
class EvictManager {
public:
    struct Config {
        uint32_t check_interval_sec = 60;
        double high_watermark = 0.85;
        double low_watermark = 0.70;
        uint64_t cold_threshold_ms = 300000;  // 5 minutes (kept for config compat, unused)
        uint32_t store_id = 0;
    };

    EvictManager(const Config& config,
                 StoreMetaIndex* meta_index,
                 MetaSyncClient* meta_sync_client,
                 PendingEvictQueue* pending_queue,
                 SlotAllocator* allocator);
    ~EvictManager();

    EvictManager(const EvictManager&) = delete;
    EvictManager& operator=(const EvictManager&) = delete;

    void Start();
    void Stop();

    /// Synchronous forced eviction for Put path.
    /// Evicts LRU entries until at least `needed_bytes` of alloc_size has
    /// been enqueued for reclamation. Returns the total alloc_size evicted.
    uint32_t ForceEvict(uint32_t needed_bytes);

private:
    void EvictLoop();
    bool TryEvictBatchLRU();
    bool EvictEntries(const std::vector<StoreKeyRecord>& candidates);

    Config config_;
    StoreMetaIndex* meta_index_;          // not owned
    MetaSyncClient* meta_sync_client_;    // not owned, may be nullptr
    PendingEvictQueue* pending_queue_;    // not owned
    SlotAllocator* allocator_;           // not owned

    std::thread thread_;
    std::atomic<bool> running_{false};
};

} // namespace falconkv

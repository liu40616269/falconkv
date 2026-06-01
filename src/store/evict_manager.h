#pragma once

#include <thread>
#include <atomic>
#include <cstdint>

namespace falconkv {

class StoreMetaIndex;
class MetaSyncClient;
class PendingEvictQueue;
class BuddyAllocator;

/// EvictManager runs a background thread that periodically checks SSD usage.
/// When usage exceeds the high watermark, it selects cold entries from
/// StoreMetaIndex, notifies Meta via SyncRemove, removes them from the local
/// index, and enqueues them in PendingEvictQueue for deferred space reclamation.
class EvictManager {
public:
    struct Config {
        uint32_t check_interval_sec = 60;
        double high_watermark = 0.85;
        double low_watermark = 0.70;
        uint64_t cold_threshold_ms = 300000;  // 5 minutes
        uint32_t store_id = 0;
    };

    EvictManager(const Config& config,
                 StoreMetaIndex* meta_index,
                 MetaSyncClient* meta_sync_client,
                 PendingEvictQueue* pending_queue,
                 BuddyAllocator* allocator);
    ~EvictManager();

    EvictManager(const EvictManager&) = delete;
    EvictManager& operator=(const EvictManager&) = delete;

    void Start();
    void Stop();

private:
    void EvictLoop();
    bool TryEvictBatch();

    Config config_;
    StoreMetaIndex* meta_index_;          // not owned
    MetaSyncClient* meta_sync_client_;    // not owned, may be nullptr
    PendingEvictQueue* pending_queue_;    // not owned
    BuddyAllocator* allocator_;           // not owned

    std::thread thread_;
    std::atomic<bool> running_{false};
};

} // namespace falconkv

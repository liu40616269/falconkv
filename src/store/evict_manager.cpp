#include "src/store/evict_manager.h"
#include "src/store/store_meta_index.h"
#include "src/store/meta_sync_client.h"
#include "src/store/pending_evict_queue.h"
#include "src/common/buddy_allocator.h"
#include "src/common/time_util.h"
#include "src/common/logging.h"

#include <chrono>
#include <vector>
#include <string>

namespace falconkv {

EvictManager::EvictManager(const Config& config,
                           StoreMetaIndex* meta_index,
                           MetaSyncClient* meta_sync_client,
                           PendingEvictQueue* pending_queue,
                           BuddyAllocator* allocator)
    : config_(config),
      meta_index_(meta_index),
      meta_sync_client_(meta_sync_client),
      pending_queue_(pending_queue),
      allocator_(allocator) {}

EvictManager::~EvictManager() {
    Stop();
}

void EvictManager::Start() {
    if (running_.exchange(true)) {
        return;
    }
    thread_ = std::thread(&EvictManager::EvictLoop, this);
}

void EvictManager::Stop() {
    if (!running_.exchange(false)) {
        return;
    }
    if (thread_.joinable()) {
        thread_.join();
    }
}

void EvictManager::EvictLoop() {
    while (running_.load()) {
        // Sleep first, then check.
        for (uint32_t i = 0; i < config_.check_interval_sec && running_.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (!running_.load()) break;

        double usage = allocator_->GetUsageRatio();
        if (usage <= config_.high_watermark) {
            continue;
        }

        LOG(WARNING) << "[EvictManager] Usage " << (usage * 100)
                     << "% exceeds high watermark " << (config_.high_watermark * 100)
                     << "%, starting eviction for store " << config_.store_id;

        // Keep evicting until usage drops below low watermark or no more
        // candidates are available.
        while (running_.load() && allocator_->GetUsageRatio() > config_.low_watermark) {
            if (!TryEvictBatch()) {
                break;
            }
        }
    }
}

bool EvictManager::TryEvictBatch() {
    uint64_t threshold = GetWallTimeMs() - config_.cold_threshold_ms;
    // Evict at most 16 entries per batch to keep latency bounded.
    auto candidates = meta_index_->GetColdEntries(threshold, 16);
    if (candidates.empty()) {
        return false;
    }

    // 1. Notify Meta server first (wait for confirmation).
    std::vector<std::string> keys;
    keys.reserve(candidates.size());
    for (const auto& rec : candidates) {
        keys.push_back(rec.key);
    }

    if (meta_sync_client_) {
        Status s = meta_sync_client_->SyncRemove(config_.store_id, keys);
        if (!s.ok()) {
            LOG(ERROR) << "[EvictManager] TryEvictBatch: SyncRemove failed for "
                       << keys.size() << " keys: " << s.ToString();
            // Meta sync failed — skip this batch to keep consistency.
            return false;
        }
    }

    // 2. Remove from local index and enqueue for deferred space reclamation.
    for (const auto& rec : candidates) {
        meta_index_->Remove(rec.key);
        pending_queue_->Enqueue(rec.key, rec.offset, rec.alloc_size);
    }

    return true;
}

} // namespace falconkv

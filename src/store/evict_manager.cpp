#include "src/store/evict_manager.h"
#include "src/store/store_meta_index.h"
#include "src/store/meta_sync_client.h"
#include "src/store/pending_evict_queue.h"
#include "src/common/slot_allocator.h"
#include "src/common/logging.h"

#include <chrono>
#include <vector>
#include <string>

namespace falconkv {

EvictManager::EvictManager(const Config& config,
                           StoreMetaIndex* meta_index,
                           MetaSyncClient* meta_sync_client,
                           PendingEvictQueue* pending_queue,
                           SlotAllocator* allocator)
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
                     << "%, starting LRU eviction for store " << config_.store_id;

        // Keep evicting until usage drops below low watermark or no more
        // candidates are available.
        while (running_.load() && allocator_->GetUsageRatio() > config_.low_watermark) {
            if (!TryEvictBatchLRU()) {
                break;
            }
        }
    }
}

bool EvictManager::TryEvictBatchLRU() {
    // Evict at most 16 entries per batch — directly from LRU tail.
    auto candidates = meta_index_->GetLRUCandidates(16);
    if (candidates.empty()) {
        return false;
    }
    return EvictEntries(candidates);
}

bool EvictManager::EvictEntries(const std::vector<StoreKeyRecord>& candidates) {
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
            LOG(ERROR) << "[EvictManager] EvictEntries: SyncRemove failed for "
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

uint32_t EvictManager::ForceEvict(uint32_t needed_bytes) {
    constexpr int kMaxRounds = 5;
    constexpr size_t kMaxPerRound = 64;

    uint32_t total_evicted = 0;

    for (int round = 0; round < kMaxRounds; ++round) {
        if (total_evicted >= needed_bytes) {
            break;
        }

        uint32_t remaining = needed_bytes - total_evicted;
        auto candidates = meta_index_->GetLRUCandidatesBySize(remaining, kMaxPerRound);
        if (candidates.entries.empty()) {
            break;
        }

        // Try to evict this batch.
        std::vector<std::string> keys;
        keys.reserve(candidates.entries.size());
        for (const auto& rec : candidates.entries) {
            keys.push_back(rec.key);
        }

        if (meta_sync_client_) {
            Status s = meta_sync_client_->SyncRemove(config_.store_id, keys);
            if (!s.ok()) {
                LOG(ERROR) << "[EvictManager] ForceEvict: SyncRemove failed for "
                           << keys.size() << " keys: " << s.ToString();
                break;
            }
        }

        // Remove from local index and enqueue.
        for (const auto& rec : candidates.entries) {
            meta_index_->Remove(rec.key);
            pending_queue_->Enqueue(rec.key, rec.offset, rec.alloc_size);
        }

        total_evicted += candidates.total_alloc_size;
    }

    if (total_evicted > 0) {
        LOG(WARNING) << "[EvictManager] ForceEvict: evicted "
                     << total_evicted << " bytes for store " << config_.store_id;
    }

    return total_evicted;
}

} // namespace falconkv

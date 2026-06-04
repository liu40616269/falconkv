#include "src/store/store_meta_index.h"

#include "src/common/time_util.h"

#include <algorithm>

namespace falconkv {

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
StoreMetaIndex::StoreMetaIndex() {
    sentinel_.lru_prev = &sentinel_;
    sentinel_.lru_next = &sentinel_;
}

// ---------------------------------------------------------------------------
// LRU list helpers (must be called with mutex_ held)
// ---------------------------------------------------------------------------
void StoreMetaIndex::LruRemove(StoreKeyRecord* rec) {
    rec->lru_prev->lru_next = rec->lru_next;
    rec->lru_next->lru_prev = rec->lru_prev;
    rec->lru_prev = nullptr;
    rec->lru_next = nullptr;
}

void StoreMetaIndex::LruPushFront(StoreKeyRecord* rec) {
    // Insert right after sentinel_ (MRU end).
    rec->lru_next = sentinel_.lru_next;
    rec->lru_prev = &sentinel_;
    sentinel_.lru_next->lru_prev = rec;
    sentinel_.lru_next = rec;
}

// ---------------------------------------------------------------------------
// Put
// ---------------------------------------------------------------------------
void StoreMetaIndex::Put(const std::string& key, const StoreKeyRecord& record) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = index_.find(key);
    if (it != index_.end()) {
        // Update existing entry.
        StoreKeyRecord* rec = it->second.get();
        bool was_committed = (rec->stat == 1);

        // Unlink from LRU if previously committed.
        if (was_committed) {
            LruRemove(rec);
            --committed_count_;
        }

        // Copy all fields except LRU pointers.
        rec->offset = record.offset;
        rec->size = record.size;
        rec->alloc_size = record.alloc_size;
        rec->stat = record.stat;
        rec->access_time_ms = record.access_time_ms;

        // If now committed, link into LRU at MRU end.
        if (rec->stat == 1) {
            LruPushFront(rec);
            ++committed_count_;
        }
    } else {
        // New entry.
        auto rec_ptr = std::make_unique<StoreKeyRecord>(record);
        rec_ptr->lru_prev = nullptr;
        rec_ptr->lru_next = nullptr;
        if (rec_ptr->stat == 1) {
            LruPushFront(rec_ptr.get());
            ++committed_count_;
        }
        index_[key] = std::move(rec_ptr);
    }
}

// ---------------------------------------------------------------------------
// Get — auto-touch (move to MRU end)
// ---------------------------------------------------------------------------
std::optional<StoreKeyRecord> StoreMetaIndex::Get(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = index_.find(key);
    if (it == index_.end() || it->second->stat != 1) {
        return std::nullopt;
    }
    StoreKeyRecord* rec = it->second.get();
    // Touch: move to MRU end.
    LruRemove(rec);
    LruPushFront(rec);
    rec->access_time_ms = GetWallTimeMs();
    return *rec;
}

// ---------------------------------------------------------------------------
// BatchContains — each hit moves to MRU end
// ---------------------------------------------------------------------------
void StoreMetaIndex::BatchContains(const std::vector<std::string>& keys,
                                    std::vector<StoreKeyRecord>& hits,
                                    std::vector<std::string>& misses) {
    hits.clear();
    misses.clear();

    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t now = GetWallTimeMs();

    for (const auto& key : keys) {
        auto it = index_.find(key);
        if (it != index_.end() && it->second->stat == 1) {
            StoreKeyRecord* rec = it->second.get();
            // Touch: move to MRU end.
            LruRemove(rec);
            LruPushFront(rec);
            rec->access_time_ms = now;
            hits.push_back(*rec);
        } else {
            misses.push_back(key);
        }
    }
}

// ---------------------------------------------------------------------------
// Commit — stat 0→1, link into LRU at MRU end
// ---------------------------------------------------------------------------
bool StoreMetaIndex::Commit(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = index_.find(key);
    if (it == index_.end()) {
        return false;
    }
    StoreKeyRecord* rec = it->second.get();
    if (rec->stat == 1) {
        return true;  // already committed
    }
    rec->stat = 1;
    rec->access_time_ms = GetWallTimeMs();
    LruPushFront(rec);
    ++committed_count_;
    return true;
}

// ---------------------------------------------------------------------------
// Remove — unlink from LRU if committed
// ---------------------------------------------------------------------------
std::optional<StoreKeyRecord> StoreMetaIndex::Remove(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = index_.find(key);
    if (it == index_.end()) {
        return std::nullopt;
    }
    StoreKeyRecord* rec = it->second.get();
    if (rec->stat == 1) {
        LruRemove(rec);
        --committed_count_;
    }
    StoreKeyRecord record = std::move(*rec);
    index_.erase(it);
    return record;
}

// ---------------------------------------------------------------------------
// Touch — move to MRU end and update access_time_ms
// ---------------------------------------------------------------------------
void StoreMetaIndex::Touch(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = index_.find(key);
    if (it != index_.end() && it->second->stat == 1) {
        StoreKeyRecord* rec = it->second.get();
        LruRemove(rec);
        LruPushFront(rec);
        rec->access_time_ms = GetWallTimeMs();
    }
}

// ---------------------------------------------------------------------------
// CommittedCount — O(1)
// ---------------------------------------------------------------------------
size_t StoreMetaIndex::CommittedCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return committed_count_;
}

// ---------------------------------------------------------------------------
// GetAllCommittedEntries
// ---------------------------------------------------------------------------
std::vector<StoreKeyRecord> StoreMetaIndex::GetAllCommittedEntries() const {
    std::vector<StoreKeyRecord> result;
    std::lock_guard<std::mutex> lock(mutex_);
    result.reserve(committed_count_);
    for (const auto& [_, rec_ptr] : index_) {
        if (rec_ptr->stat == 1) {
            result.push_back(*rec_ptr);
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// GetLRUCandidates — O(k) from LRU tail
// ---------------------------------------------------------------------------
std::vector<StoreKeyRecord> StoreMetaIndex::GetLRUCandidates(
    size_t max_count) const {
    std::vector<StoreKeyRecord> result;
    std::lock_guard<std::mutex> lock(mutex_);
    result.reserve(max_count);
    // Walk from LRU end (sentinel_.lru_prev) toward MRU end.
    StoreKeyRecord* cur = sentinel_.lru_prev;
    while (cur != &sentinel_ && result.size() < max_count) {
        result.push_back(*cur);
        cur = cur->lru_prev;
    }
    return result;
}

// ---------------------------------------------------------------------------
// GetLRUCandidatesBySize — accumulate until total_alloc_size >= needed_bytes
// ---------------------------------------------------------------------------
EvictCandidates StoreMetaIndex::GetLRUCandidatesBySize(
    uint32_t needed_bytes, size_t max_count) const {
    EvictCandidates result;
    std::lock_guard<std::mutex> lock(mutex_);
    result.entries.reserve(max_count);
    StoreKeyRecord* cur = sentinel_.lru_prev;
    while (cur != &sentinel_ &&
           result.entries.size() < max_count &&
           result.total_alloc_size < needed_bytes) {
        result.entries.push_back(*cur);
        result.total_alloc_size += cur->alloc_size;
        cur = cur->lru_prev;
    }
    return result;
}

// ---------------------------------------------------------------------------
// GetColdEntries — kept for backward compatibility
// ---------------------------------------------------------------------------
std::vector<StoreKeyRecord> StoreMetaIndex::GetColdEntries(
    uint64_t access_time_threshold, size_t max_count) const {
    std::vector<StoreKeyRecord> candidates;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [_, rec_ptr] : index_) {
            if (rec_ptr->stat == 1 && rec_ptr->access_time_ms <= access_time_threshold) {
                candidates.push_back(*rec_ptr);
            }
        }
    }
    // Sort by access_time ascending (coldest first)
    std::sort(candidates.begin(), candidates.end(),
              [](const StoreKeyRecord& a, const StoreKeyRecord& b) {
                  return a.access_time_ms < b.access_time_ms;
              });
    if (candidates.size() > max_count) {
        candidates.resize(max_count);
    }
    return candidates;
}

} // namespace falconkv

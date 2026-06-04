#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <optional>
#include <mutex>
#include <cstdint>

namespace falconkv {

struct StoreKeyRecord {
    std::string key;
    uint64_t offset = 0;
    uint32_t size = 0;
    uint32_t alloc_size = 0;
    int stat = 0;             // 0=allocated, 1=committed, 2=evict
    uint64_t access_time_ms = 0;

    // LRU doubly-linked list pointers (managed by StoreMetaIndex)
    StoreKeyRecord* lru_prev = nullptr;
    StoreKeyRecord* lru_next = nullptr;
};

/// Result set for size-aware LRU eviction candidate selection.
struct EvictCandidates {
    std::vector<StoreKeyRecord> entries;
    uint32_t total_alloc_size = 0;
};

/// Hash map + doubly-linked list LRU index.
///
/// Only entries with stat == 1 (committed) are linked into the LRU list.
/// sentinel_ is a dummy node that acts as the list boundary:
///   sentinel_.lru_next → MRU end (most recently used)
///   sentinel_.lru_prev → LRU end (eviction candidate)
///
/// index_ holds unique_ptr<StoreKeyRecord> so that pointers remain stable
/// across map rehashes.
class StoreMetaIndex {
public:
    StoreMetaIndex();
    ~StoreMetaIndex() = default;

    // Non-copyable
    StoreMetaIndex(const StoreMetaIndex&) = delete;
    StoreMetaIndex& operator=(const StoreMetaIndex&) = delete;

    /// Insert or update a key record.
    /// If record.stat == 1, the entry is moved to the MRU end.
    void Put(const std::string& key, const StoreKeyRecord& record);

    /// Look up a single key. Returns nullopt if not found or not committed.
    /// Automatically moves the entry to the MRU end (LRU touch).
    std::optional<StoreKeyRecord> Get(const std::string& key);

    /// Batch query: fills `hits` with found committed records,
    /// fills `misses` with keys not found (or not committed).
    /// Each hit is moved to the MRU end.
    void BatchContains(const std::vector<std::string>& keys,
                       std::vector<StoreKeyRecord>& hits,
                       std::vector<std::string>& misses);

    /// Mark a key as committed (stat 0 -> 1). Returns false if not found.
    /// The entry is linked into the LRU list at the MRU end.
    bool Commit(const std::string& key);

    /// Remove a key record. Returns the removed record (or nullopt).
    /// Unlinks from the LRU list if committed.
    std::optional<StoreKeyRecord> Remove(const std::string& key);

    /// Update access time and move to MRU end (LRU touch).
    void Touch(const std::string& key);

    /// Return the number of committed entries. O(1).
    size_t CommittedCount() const;

    /// Return all committed entries (stat == 1), used for full resync.
    std::vector<StoreKeyRecord> GetAllCommittedEntries() const;

    /// Return up to max_count entries from the LRU end (coldest).
    /// O(k) where k = max_count.
    std::vector<StoreKeyRecord> GetLRUCandidates(size_t max_count) const;

    /// Accumulate eviction candidates from the LRU end until total alloc_size
    /// >= needed_bytes or max_count entries are collected.
    EvictCandidates GetLRUCandidatesBySize(uint32_t needed_bytes,
                                            size_t max_count) const;

    /// Return cold entries (stat==1, access_time_ms <= threshold) sorted by
    /// access_time ascending. Kept for backward compatibility.
    std::vector<StoreKeyRecord> GetColdEntries(uint64_t access_time_threshold,
                                                size_t max_count) const;

private:
    // --- LRU list helpers (must be called with mutex_ held) ---
    void LruRemove(StoreKeyRecord* rec);
    void LruPushFront(StoreKeyRecord* rec);  // push to MRU end

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::unique_ptr<StoreKeyRecord>> index_;

    // Sentinel node: lru_next = MRU end, lru_prev = LRU end.
    // Empty list: sentinel_.lru_next == sentinel_.lru_prev == &sentinel_
    StoreKeyRecord sentinel_;

    // Number of committed (stat == 1) entries currently in the LRU list.
    size_t committed_count_ = 0;
};

} // namespace falconkv

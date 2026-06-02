#pragma once

#include <string>
#include <vector>
#include <unordered_map>
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
};

class StoreMetaIndex {
public:
    StoreMetaIndex() = default;
    ~StoreMetaIndex() = default;

    // Non-copyable
    StoreMetaIndex(const StoreMetaIndex&) = delete;
    StoreMetaIndex& operator=(const StoreMetaIndex&) = delete;

    /// Insert or update a key record.
    void Put(const std::string& key, const StoreKeyRecord& record);

    /// Look up a single key. Returns nullopt if not found or evicted.
    std::optional<StoreKeyRecord> Get(const std::string& key);

    /// Batch query: fills `hits` with found committed records,
    /// fills `misses` with keys not found (or not committed).
    void BatchContains(const std::vector<std::string>& keys,
                       std::vector<StoreKeyRecord>& hits,
                       std::vector<std::string>& misses);

    /// Mark a key as committed (stat 0 -> 1). Returns false if not found.
    bool Commit(const std::string& key);

    /// Remove a key record. Returns the removed record (or nullopt).
    std::optional<StoreKeyRecord> Remove(const std::string& key);

    /// Update access time for a key (LRU touch).
    void Touch(const std::string& key);

    /// Return the number of committed entries.
    size_t CommittedCount() const;

    /// Return all committed entries (stat == 1), used for full resync.
    std::vector<StoreKeyRecord> GetAllCommittedEntries() const;

    /// Return cold entries (stat==1, access_time_ms <= threshold) sorted by
    /// access_time ascending.  Used by EvictManager to select eviction
    /// candidates.
    std::vector<StoreKeyRecord> GetColdEntries(uint64_t access_time_threshold,
                                                size_t max_count) const;

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, StoreKeyRecord> index_;
};

} // namespace falconkv

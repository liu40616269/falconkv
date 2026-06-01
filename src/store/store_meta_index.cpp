#include "src/store/store_meta_index.h"

#include "src/common/time_util.h"

#include <algorithm>

namespace falconkv {

void StoreMetaIndex::Put(const std::string& key, const StoreKeyRecord& record) {
    std::lock_guard<std::mutex> lock(mutex_);
    index_[key] = record;
}

std::optional<StoreKeyRecord> StoreMetaIndex::Get(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = index_.find(key);
    if (it == index_.end() || it->second.stat != 1) {
        return std::nullopt;
    }
    return it->second;
}

void StoreMetaIndex::BatchContains(const std::vector<std::string>& keys,
                                    std::vector<StoreKeyRecord>& hits,
                                    std::vector<std::string>& misses) {
    hits.clear();
    misses.clear();

    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t now = GetWallTimeMs();

    for (const auto& key : keys) {
        auto it = index_.find(key);
        if (it != index_.end() && it->second.stat == 1) {
            it->second.access_time_ms = now;
            hits.push_back(it->second);
        } else {
            misses.push_back(key);
        }
    }
}

bool StoreMetaIndex::Commit(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = index_.find(key);
    if (it == index_.end()) {
        return false;
    }
    it->second.stat = 1;
    it->second.access_time_ms = GetWallTimeMs();
    return true;
}

std::optional<StoreKeyRecord> StoreMetaIndex::Remove(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = index_.find(key);
    if (it == index_.end()) {
        return std::nullopt;
    }
    StoreKeyRecord record = std::move(it->second);
    index_.erase(it);
    return record;
}

void StoreMetaIndex::Touch(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = index_.find(key);
    if (it != index_.end()) {
        it->second.access_time_ms = GetWallTimeMs();
    }
}

size_t StoreMetaIndex::CommittedCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t count = 0;
    for (const auto& [_, rec] : index_) {
        if (rec.stat == 1) {
            ++count;
        }
    }
    return count;
}

std::vector<StoreKeyRecord> StoreMetaIndex::GetAllCommittedEntries() const {
    std::vector<StoreKeyRecord> result;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [_, rec] : index_) {
        if (rec.stat == 1) {
            result.push_back(rec);
        }
    }
    return result;
}

std::vector<StoreKeyRecord> StoreMetaIndex::GetColdEntries(
    uint64_t access_time_threshold, size_t max_count) const {
    std::vector<StoreKeyRecord> candidates;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [_, rec] : index_) {
            if (rec.stat == 1 && rec.access_time_ms <= access_time_threshold) {
                candidates.push_back(rec);
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

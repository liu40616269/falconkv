#include "client/key_desc_cache.h"

#include <chrono>
#include <algorithm>

namespace falconkv {

KeyDescCache::KeyDescCache(size_t capacity)
    : capacity_(capacity) {}

std::optional<KeyDescriptor> KeyDescCache::Lookup(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = cache_.find(key);
    if (it == cache_.end()) {
        return std::nullopt;
    }
    // Move key to the back of the LRU list (most recently used)
    lru_list_.remove(key);
    lru_list_.push_back(key);
    // Update access time
    auto now = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
    it->second.access_time_ms = static_cast<uint64_t>(ms);
    return it->second;
}

int KeyDescCache::BatchLookup(const std::vector<std::string>& keys,
                               std::vector<KeyDescriptor>& hit_descs,
                               std::vector<std::string>& missing_keys) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto now = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();

    hit_descs.clear();
    missing_keys.clear();

    for (const auto& key : keys) {
        auto it = cache_.find(key);
        if (it != cache_.end()) {
            // Move key to the back of the LRU list
            lru_list_.remove(key);
            lru_list_.push_back(key);
            // Update access time
            it->second.access_time_ms = static_cast<uint64_t>(ms);
            hit_descs.push_back(it->second);
        } else {
            missing_keys.push_back(key);
        }
    }

    return static_cast<int>(hit_descs.size());
}

void KeyDescCache::Insert(const std::string& key, const KeyDescriptor& desc) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = cache_.find(key);
    if (it != cache_.end()) {
        // Key already exists, update value and move to back of LRU
        it->second = desc;
        lru_list_.remove(key);
        lru_list_.push_back(key);
    } else {
        EvictIfNeeded();
        cache_[key] = desc;
        lru_list_.push_back(key);
    }
}

void KeyDescCache::BatchInsert(
    const std::vector<std::pair<std::string, KeyDescriptor>>& items) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [key, desc] : items) {
        auto it = cache_.find(key);
        if (it != cache_.end()) {
            it->second = desc;
            lru_list_.remove(key);
            lru_list_.push_back(key);
        } else {
            EvictIfNeeded();
            cache_[key] = desc;
            lru_list_.push_back(key);
        }
    }
}

void KeyDescCache::Invalidate(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = cache_.find(key);
    if (it != cache_.end()) {
        lru_list_.remove(key);
        cache_.erase(it);
    }
}

void KeyDescCache::BatchInvalidate(const std::vector<std::string>& keys) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& key : keys) {
        auto it = cache_.find(key);
        if (it != cache_.end()) {
            lru_list_.remove(key);
            cache_.erase(it);
        }
    }
}

size_t KeyDescCache::Size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cache_.size();
}

void KeyDescCache::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    cache_.clear();
    lru_list_.clear();
}

void KeyDescCache::EvictIfNeeded() {
    // Caller must hold mutex_
    while (cache_.size() >= capacity_ && !lru_list_.empty()) {
        const std::string& oldest_key = lru_list_.front();
        cache_.erase(oldest_key);
        lru_list_.pop_front();
    }
}

} // namespace falconkv

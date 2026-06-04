#include "store/fd_cache.h"

#include <chrono>
#include <fcntl.h>
#include <unistd.h>

#include "src/common/logging.h"

namespace falconkv {

FdCache::~FdCache() {
    CloseAll();
}

FdCache::FdEntry& FdCache::GetOrCreate(const std::string& data_file) {
    auto it = fd_map_.find(data_file);
    if (it != fd_map_.end()) {
        auto now = std::chrono::steady_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count();
        it->second.last_access_ms = static_cast<uint64_t>(ms);
        return it->second;
    }

    // Open buffered fd first (always succeeds or fails together).
    int buf_fd = ::open(data_file.c_str(), O_RDWR, 0644);
    if (buf_fd < 0) {
        // Insert an entry with both fds invalid to avoid retrying.
        auto now = std::chrono::steady_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count();
        auto [ins_it, _] = fd_map_.emplace(data_file,
            FdEntry{-1, -1, static_cast<uint64_t>(ms)});
        return ins_it->second;
    }

    // Try to open O_DIRECT fd.
    int direct_fd = ::open(data_file.c_str(), O_DIRECT | O_RDWR, 0644);
    if (direct_fd < 0) {
        LOG(INFO) << "[FdCache] O_DIRECT not available for " << data_file
                  << ", using buffered only";
        direct_fd = -1;
    }

    auto now = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();

    auto [ins_it, _] = fd_map_.emplace(data_file,
        FdEntry{direct_fd, buf_fd, static_cast<uint64_t>(ms)});
    return ins_it->second;
}

int FdCache::GetFd(const std::string& data_file) {
    std::lock_guard<std::mutex> lock(mutex_);
    FdEntry& entry = GetOrCreate(data_file);
    // Prefer O_DIRECT fd; fall back to buffered fd.
    return (entry.direct_fd >= 0) ? entry.direct_fd : entry.buffered_fd;
}

int FdCache::GetBufferedFd(const std::string& data_file) {
    std::lock_guard<std::mutex> lock(mutex_);
    FdEntry& entry = GetOrCreate(data_file);
    return entry.buffered_fd;
}

void FdCache::EvictIdle(size_t idle_threshold_ms) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto now = std::chrono::steady_clock::now();
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();

    auto it = fd_map_.begin();
    while (it != fd_map_.end()) {
        uint64_t elapsed = static_cast<uint64_t>(now_ms) - it->second.last_access_ms;
        if (elapsed > idle_threshold_ms) {
            if (it->second.direct_fd >= 0) {
                ::close(it->second.direct_fd);
            }
            if (it->second.buffered_fd >= 0) {
                ::close(it->second.buffered_fd);
            }
            it = fd_map_.erase(it);
        } else {
            ++it;
        }
    }
}

void FdCache::CloseAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [path, entry] : fd_map_) {
        if (entry.direct_fd >= 0) {
            ::close(entry.direct_fd);
        }
        if (entry.buffered_fd >= 0) {
            ::close(entry.buffered_fd);
        }
    }
    fd_map_.clear();
}

} // namespace falconkv

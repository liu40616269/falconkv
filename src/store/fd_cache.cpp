#include "store/fd_cache.h"

#include <chrono>
#include <fcntl.h>
#include <unistd.h>
#include <system_error>

namespace falconkv {

FdCache::~FdCache() {
    CloseAll();
}

int FdCache::GetFd(const std::string& data_file) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = fd_map_.find(data_file);
    if (it != fd_map_.end()) {
        // Update last access time
        auto now = std::chrono::steady_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count();
        it->second.last_access_ms = static_cast<uint64_t>(ms);
        return it->second.fd;
    }

    // Open file with O_DIRECT | O_RDWR
    int fd = ::open(data_file.c_str(), O_DIRECT | O_RDWR, 0644);
    if (fd < 0) {
        // If O_DIRECT fails (e.g., filesystem does not support it),
        // fall back to O_RDWR only
        fd = ::open(data_file.c_str(), O_RDWR, 0644);
        if (fd < 0) {
            return -1;
        }
    }

    auto now = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();

    fd_map_.emplace(data_file, FdEntry{fd, static_cast<uint64_t>(ms)});
    return fd;
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
            ::close(it->second.fd);
            it = fd_map_.erase(it);
        } else {
            ++it;
        }
    }
}

void FdCache::CloseAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [path, entry] : fd_map_) {
        ::close(entry.fd);
    }
    fd_map_.clear();
}

} // namespace falconkv

#pragma once
#include <string>
#include <unordered_map>
#include <mutex>
#include <cstdint>

namespace falconkv {

class FdCache {
public:
    FdCache() = default;
    ~FdCache();

    int GetFd(const std::string& data_file);
    void EvictIdle(size_t idle_threshold_ms);
    void CloseAll();

private:
    struct FdEntry {
        int fd;
        uint64_t last_access_ms;
    };
    std::mutex mutex_;
    std::unordered_map<std::string, FdEntry> fd_map_;
};

} // namespace falconkv

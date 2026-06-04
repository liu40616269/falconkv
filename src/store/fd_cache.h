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

    /// Get or create the O_DIRECT fd for the given file.
    /// Falls back to buffered-only if O_DIRECT is unavailable.
    int GetFd(const std::string& data_file);

    /// Get or create the buffered fd for the given file.
    int GetBufferedFd(const std::string& data_file);

    void EvictIdle(size_t idle_threshold_ms);
    void CloseAll();

private:
    struct FdEntry {
        int direct_fd;       // O_DIRECT fd (-1 if unavailable)
        int buffered_fd;     // Buffered fd (always valid)
        uint64_t last_access_ms;
    };

    FdEntry& GetOrCreate(const std::string& data_file);

    std::mutex mutex_;
    std::unordered_map<std::string, FdEntry> fd_map_;
};

} // namespace falconkv

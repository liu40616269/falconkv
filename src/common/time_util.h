#pragma once
#include <cstdint>
#include <chrono>
#include <ctime>
#include <string>

namespace falconkv {

inline uint64_t GetCurrentTimeNs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
}

inline uint64_t GetCurrentTimeMs() {
    return GetCurrentTimeNs() / 1000000ULL;
}

inline uint64_t GetCurrentTimeUs() {
    return GetCurrentTimeNs() / 1000ULL;
}

inline uint64_t GetWallTimeMs() {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch());
    return static_cast<uint64_t>(ms.count());
}

} // namespace falconkv

#pragma once

#include <vector>
#include <cstdint>

#include "src/common/status.h"

namespace falconkv {

struct UringIORequest {
    uint64_t offset = 0;          // IO offset (must be aligned if fd is O_DIRECT)
    uint32_t size = 0;            // IO size (must be aligned if fd is O_DIRECT)
    void* buffer = nullptr;       // IO buffer (must be aligned if fd is O_DIRECT)
    // Per-request fd: -1 means use the batch-level fd passed to BatchRead/BatchWrite.
    // When >= 0, this fd overrides the batch-level fd for this specific request.
    int fd = -1;
};

struct UringIOResult {
    int index;                    // original request index
    Status status;
};

class IOUringEngine {
public:
    struct Config {
        uint32_t queue_depth = 128;
    };

    IOUringEngine();
    ~IOUringEngine();

    IOUringEngine(const IOUringEngine&) = delete;
    IOUringEngine& operator=(const IOUringEngine&) = delete;

    bool Init(const Config& config);
    void Close();
    bool available() const;

    // Batch write: block until all complete.
    std::vector<UringIOResult> BatchWrite(int fd,
        std::vector<UringIORequest>& requests);

    // Batch read: block until all complete.
    std::vector<UringIOResult> BatchRead(int fd,
        std::vector<UringIORequest>& requests);

private:
    bool available_ = false;
    uint32_t queue_depth_ = 128;

    // Forward-declared; actual struct io_uring is only known inside the .cpp
    // where liburing.h is included. We store it as void* here and cast.
    void* ring_ = nullptr;
};

} // namespace falconkv

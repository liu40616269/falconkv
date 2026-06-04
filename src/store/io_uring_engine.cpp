#include "src/store/io_uring_engine.h"
#include "src/common/logging.h"

#include <unistd.h>
#include <cstring>
#include <algorithm>

#ifdef FALCONKV_HAS_IOURING
#include <liburing.h>
#endif

namespace falconkv {

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------
IOUringEngine::IOUringEngine() = default;

IOUringEngine::~IOUringEngine() {
    Close();
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
bool IOUringEngine::Init(const Config& config) {
#ifndef FALCONKV_HAS_IOURING
    (void)config;
    LOG(INFO) << "[IOUringEngine] Compiled without liburing support, io_uring disabled";
    available_ = false;
    return false;
#else
    queue_depth_ = config.queue_depth;

    auto* ring = new struct io_uring();
    struct io_uring_params params = {};
    int ret = io_uring_queue_init_params(queue_depth_, ring, &params);
    if (ret < 0) {
        LOG(WARNING) << "[IOUringEngine] io_uring_queue_init_params failed: "
                     << strerror(-ret) << ", falling back to thread pool";
        delete ring;
        available_ = false;
        return false;
    }

    ring_ = ring;
    available_ = true;
    LOG(INFO) << "[IOUringEngine] Initialized with queue_depth=" << queue_depth_;
    return true;
#endif
}

// ---------------------------------------------------------------------------
// Close
// ---------------------------------------------------------------------------
void IOUringEngine::Close() {
    if (!available_) return;

#ifdef FALCONKV_HAS_IOURING
    auto* ring = static_cast<struct io_uring*>(ring_);
    if (ring) {
        io_uring_queue_exit(ring);
        delete ring;
        ring_ = nullptr;
    }
#endif
    available_ = false;
    LOG(INFO) << "[IOUringEngine] Closed";
}

bool IOUringEngine::available() const {
    return available_;
}

// ---------------------------------------------------------------------------
// BatchWrite
// ---------------------------------------------------------------------------
std::vector<UringIOResult> IOUringEngine::BatchWrite(int fd,
    std::vector<UringIORequest>& requests) {
    std::vector<UringIOResult> results(requests.size());

#ifndef FALCONKV_HAS_IOURING
    for (size_t i = 0; i < requests.size(); ++i) {
        results[i] = {static_cast<int>(i),
                      Status::NotSupported("io_uring not compiled")};
    }
    return results;
#else
    if (!available_) {
        for (size_t i = 0; i < requests.size(); ++i) {
            results[i] = {static_cast<int>(i),
                          Status::IoError("io_uring engine not available")};
        }
        return results;
    }

    auto* ring = static_cast<struct io_uring*>(ring_);

    // Submit in sub-batches limited by queue_depth
    size_t idx = 0;
    while (idx < requests.size()) {
        size_t batch_start = idx;
        size_t batch_end = std::min(idx + queue_depth_, requests.size());

        // Submit SQEs for this sub-batch
        unsigned submitted = 0;
        for (size_t i = batch_start; i < batch_end; ++i) {
            struct io_uring_sqe* sqe = io_uring_get_sqe(ring);
            if (!sqe) {
                // Ring is full, submit what we have so far and get a new SQE
                break;
            }
            int effective_fd = (requests[i].fd >= 0) ? requests[i].fd : fd;
            io_uring_prep_write(sqe, effective_fd, requests[i].buffer,
                                requests[i].size, requests[i].offset);
            sqe->user_data = i;
            ++submitted;
            idx = i + 1;
        }

        if (submitted == 0) break;

        int ret = io_uring_submit(ring);
        if (ret < 0) {
            for (size_t i = batch_start; i < idx; ++i) {
                results[i] = {static_cast<int>(i),
                              Status::IoError("io_uring_submit failed: " +
                                              std::string(strerror(-ret)))};
            }
            continue;
        }

        // Reap CQEs
        for (unsigned c = 0; c < submitted; ++c) {
            struct io_uring_cqe* cqe = nullptr;
            ret = io_uring_wait_cqe(ring, &cqe);
            if (ret < 0) {
                // Shouldn't happen, but handle gracefully
                continue;
            }
            size_t req_idx = cqe->user_data;
            if (cqe->res < 0) {
                results[req_idx] = {
                    static_cast<int>(req_idx),
                    Status::IoError("io_uring write failed: " +
                                    std::string(strerror(-cqe->res)))};
            } else if (static_cast<unsigned>(cqe->res) < requests[req_idx].size) {
                results[req_idx] = {
                    static_cast<int>(req_idx),
                    Status::IoError("Short write via io_uring: expected " +
                                    std::to_string(requests[req_idx].size) +
                                    " got " + std::to_string(cqe->res))};
            } else {
                results[req_idx] = {static_cast<int>(req_idx), Status::OK()};
            }
            io_uring_cqe_seen(ring, cqe);
        }
    }

    return results;
#endif
}

// ---------------------------------------------------------------------------
// BatchRead
// ---------------------------------------------------------------------------
std::vector<UringIOResult> IOUringEngine::BatchRead(int fd,
    std::vector<UringIORequest>& requests) {
    std::vector<UringIOResult> results(requests.size());

#ifndef FALCONKV_HAS_IOURING
    for (size_t i = 0; i < requests.size(); ++i) {
        results[i] = {static_cast<int>(i),
                      Status::NotSupported("io_uring not compiled")};
    }
    return results;
#else
    if (!available_) {
        for (size_t i = 0; i < requests.size(); ++i) {
            results[i] = {static_cast<int>(i),
                          Status::IoError("io_uring engine not available")};
        }
        return results;
    }

    auto* ring = static_cast<struct io_uring*>(ring_);

    // Submit in sub-batches
    size_t idx = 0;
    while (idx < requests.size()) {
        unsigned submitted = 0;
        size_t batch_start = idx;
        for (size_t i = batch_start; i < requests.size() && submitted < queue_depth_; ++i) {
            struct io_uring_sqe* sqe = io_uring_get_sqe(ring);
            if (!sqe) break;
            int effective_fd = (requests[i].fd >= 0) ? requests[i].fd : fd;
            io_uring_prep_read(sqe, effective_fd, requests[i].buffer,
                               requests[i].size, requests[i].offset);
            sqe->user_data = i;
            ++submitted;
            idx = i + 1;
        }

        if (submitted == 0) break;

        int ret = io_uring_submit(ring);
        if (ret < 0) {
            for (size_t i = batch_start; i < idx; ++i) {
                results[i] = {static_cast<int>(i),
                              Status::IoError("io_uring_submit failed: " +
                                              std::string(strerror(-ret)))};
            }
            continue;
        }

        // Reap CQEs
        for (unsigned c = 0; c < submitted; ++c) {
            struct io_uring_cqe* cqe = nullptr;
            ret = io_uring_wait_cqe(ring, &cqe);
            if (ret < 0) continue;
            size_t req_idx = cqe->user_data;
            if (cqe->res < 0) {
                results[req_idx] = {
                    static_cast<int>(req_idx),
                    Status::IoError("io_uring read failed: " +
                                    std::string(strerror(-cqe->res)))};
            } else if (static_cast<unsigned>(cqe->res) < requests[req_idx].size) {
                results[req_idx] = {
                    static_cast<int>(req_idx),
                    Status::IoError("Short read via io_uring: expected " +
                                    std::to_string(requests[req_idx].size) +
                                    " got " + std::to_string(cqe->res))};
            } else {
                results[req_idx] = {static_cast<int>(req_idx), Status::OK()};
            }
            io_uring_cqe_seen(ring, cqe);
        }
    }

    return results;
#endif
}

} // namespace falconkv

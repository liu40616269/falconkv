#include "src/store/node_local_accessor.h"

#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <memory>

#include "src/store/fd_cache.h"
#include "src/store/io_thread_pool.h"
#include "src/store/io_uring_engine.h"
#include "src/common/aligned_allocator.h"
#include "src/common/logging.h"

namespace falconkv {

static inline bool IsAlignedValue(uint64_t value, size_t alignment) {
    return (value & (alignment - 1)) == 0;
}

static inline bool IsAlignedPtr(const void* ptr, size_t alignment) {
    return (reinterpret_cast<uintptr_t>(ptr) & (alignment - 1)) == 0;
}

NodeLocalAccessor::NodeLocalAccessor()
    : fd_cache_(std::make_unique<FdCache>()) {}

NodeLocalAccessor::~NodeLocalAccessor() {
    Close();
}

void NodeLocalAccessor::InitIOEngines(const Config& config) {
    page_size_ = config.page_size;
    io_uring_enabled_ = config.io_uring_enabled;

    // Always create thread pool as fallback
    io_pool_ = std::make_unique<IOThreadPool>(config.io_threads);
    io_pool_->Start();

    // Optionally initialize io_uring engine
    if (io_uring_enabled_) {
        io_uring_engine_ = std::make_unique<IOUringEngine>();
        IOUringEngine::Config uring_cfg;
        uring_cfg.queue_depth = config.io_uring_queue_depth;
        if (!io_uring_engine_->Init(uring_cfg)) {
            LOG(WARNING) << "[NodeLocalAccessor] io_uring init failed, "
                         << "falling back to thread pool for batch reads";
            io_uring_engine_.reset();
            io_uring_enabled_ = false;
        } else {
            LOG(INFO) << "[NodeLocalAccessor] io_uring engine initialized, "
                      << "queue_depth=" << config.io_uring_queue_depth;
        }
    }

    LOG(INFO) << "[NodeLocalAccessor] IO engines initialized: "
              << "io_threads=" << config.io_threads
              << ", io_uring=" << (io_uring_enabled_ ? "enabled" : "disabled");
}

bool NodeLocalAccessor::RegisterStoreFile(uint32_t store_id,
                                           const std::string& data_file) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = store_files_.find(store_id);
    if (it != store_files_.end()) {
        if (it->second == data_file) {
            return false;  // duplicate, no action needed
        }
        it->second = data_file;
        LOG(INFO) << "[NodeLocalAccessor] RegisterStoreFile updated: store_id="
                  << store_id << ", data_file=" << data_file;
        return true;
    }
    store_files_[store_id] = data_file;
    LOG(INFO) << "[NodeLocalAccessor] RegisterStoreFile: store_id=" << store_id
              << ", data_file=" << data_file;
    return true;
}

std::string NodeLocalAccessor::GetStoreFilePath(uint32_t store_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = store_files_.find(store_id);
    if (it == store_files_.end()) {
        return "";
    }
    return it->second;
}

int NodeLocalAccessor::GetDirectFd(uint32_t store_id) {
    std::string path = GetStoreFilePath(store_id);
    if (path.empty()) {
        return -1;
    }
    return fd_cache_->GetFd(path);
}

int NodeLocalAccessor::GetBufferedFd(uint32_t store_id) {
    std::string path = GetStoreFilePath(store_id);
    if (path.empty()) {
        return -1;
    }
    return fd_cache_->GetBufferedFd(path);
}

Status NodeLocalAccessor::Write(uint32_t store_id, uint64_t offset,
                                 const void* data, uint32_t size) {
    // Always get the buffered fd (guaranteed to be open).
    int buf_fd = GetBufferedFd(store_id);
    if (buf_fd < 0) {
        LOG(ERROR) << "[NodeLocalAccessor] Write: no file for store_id " << store_id;
        return Status::IoError("NodeLocalAccessor: no file for store_id " +
                               std::to_string(store_id));
    }

    // Select fd: use O_DIRECT fd when all alignment conditions are met,
    // otherwise fall back to the buffered fd.
    int fd = buf_fd;
    int direct_fd = GetDirectFd(store_id);
    if (direct_fd >= 0 && direct_fd != buf_fd &&
        IsAlignedValue(offset, page_size_) &&
        IsAlignedValue(size, page_size_) &&
        IsAlignedPtr(data, page_size_)) {
        fd = direct_fd;
    }

    ssize_t written = ::pwrite(fd, data, size, static_cast<off_t>(offset));
    if (written < 0) {
        LOG(ERROR) << "[NodeLocalAccessor] Write: pwrite failed for store_id "
                   << store_id << ", offset=" << offset << ": " << strerror(errno);
        return Status::IoError("NodeLocalAccessor: pwrite failed for store_id " +
                               std::to_string(store_id));
    }
    if (static_cast<size_t>(written) < size) {
        LOG(ERROR) << "[NodeLocalAccessor] Write: partial write for store_id "
                   << store_id << ", expected=" << size << " written=" << written;
        return Status::IoError("NodeLocalAccessor: partial write for store_id " +
                               std::to_string(store_id));
    }

    return Status::OK();
}

Status NodeLocalAccessor::Read(uint32_t store_id, uint64_t offset,
                                void* buffer, uint32_t size) {
    int buf_fd = GetBufferedFd(store_id);
    if (buf_fd < 0) {
        LOG(ERROR) << "[NodeLocalAccessor] Read: no file for store_id " << store_id;
        return Status::IoError("NodeLocalAccessor: no file for store_id " +
                               std::to_string(store_id));
    }

    // Select fd: use O_DIRECT fd when all alignment conditions are met,
    // otherwise fall back to the buffered fd.
    int fd = buf_fd;
    int direct_fd = GetDirectFd(store_id);
    if (direct_fd >= 0 && direct_fd != buf_fd &&
        IsAlignedValue(offset, page_size_) &&
        IsAlignedValue(size, page_size_) &&
        IsAlignedPtr(buffer, page_size_)) {
        fd = direct_fd;
    }

    ssize_t nread = ::pread(fd, buffer, size, static_cast<off_t>(offset));
    if (nread < 0) {
        LOG(ERROR) << "[NodeLocalAccessor] Read: pread failed for store_id "
                   << store_id << ", offset=" << offset << ": " << strerror(errno);
        return Status::IoError("NodeLocalAccessor: pread failed for store_id " +
                               std::to_string(store_id));
    }
    if (static_cast<size_t>(nread) < size) {
        LOG(ERROR) << "[NodeLocalAccessor] Read: partial read for store_id "
                   << store_id << ", expected=" << size << " got=" << nread;
        return Status::IoError("NodeLocalAccessor: partial read for store_id " +
                               std::to_string(store_id));
    }

    return Status::OK();
}

std::vector<Status> NodeLocalAccessor::BatchRead(
    const std::vector<NodeLocalReadRequest>& requests) {
    std::vector<Status> results(requests.size(), Status::OK());
    if (requests.empty()) return results;

    // Resolve fds for all requests, selecting direct or buffered per request.
    struct ResolvedRequest {
        int fd;
        uint64_t offset;
        void* buffer;
        uint32_t size;
        bool use_direct;
    };
    std::vector<ResolvedRequest> resolved(requests.size());

    for (size_t i = 0; i < requests.size(); ++i) {
        const auto& req = requests[i];
        int buf_fd = GetBufferedFd(req.store_id);
        if (buf_fd < 0) {
            results[i] = Status::IoError(
                "NodeLocalAccessor: no file for store_id " +
                std::to_string(req.store_id));
            continue;
        }

        int direct_fd = GetDirectFd(req.store_id);
        bool use_direct = (direct_fd >= 0 && direct_fd != buf_fd &&
                           IsAlignedValue(req.offset, page_size_) &&
                           IsAlignedValue(req.size, page_size_) &&
                           IsAlignedPtr(req.buffer, page_size_));
        resolved[i] = {use_direct ? direct_fd : buf_fd,
                       req.offset, req.buffer, req.size, use_direct};
    }

    // Try io_uring path if engine is available
    if (io_uring_engine_ && io_uring_engine_->available()) {
        std::vector<UringIORequest> uring_reqs;
        uring_reqs.reserve(requests.size());
        std::vector<size_t> valid_indices;

        for (size_t i = 0; i < requests.size(); ++i) {
            if (!results[i].ok()) continue;
            valid_indices.push_back(i);

            UringIORequest uring_req;
            uring_req.offset = resolved[i].offset;
            uring_req.size = resolved[i].size;
            uring_req.buffer = resolved[i].buffer;
            uring_req.fd = resolved[i].fd;  // per-request fd
            uring_reqs.push_back(uring_req);
        }

        if (!uring_reqs.empty()) {
            // Pass -1 as batch-level fd; each request carries its own fd.
            auto uring_results = io_uring_engine_->BatchRead(
                -1, uring_reqs);
            for (size_t j = 0; j < uring_results.size(); ++j) {
                results[valid_indices[j]] = uring_results[j].status;
            }
        }
        return results;
    }

    // Thread pool fallback path
    if (io_pool_) {
        std::vector<std::function<Status()>> tasks;
        tasks.reserve(requests.size());
        std::vector<size_t> valid_indices;

        for (size_t i = 0; i < requests.size(); ++i) {
            if (!results[i].ok()) continue;
            valid_indices.push_back(i);
            auto r = resolved[i];  // copy for safe capture
            tasks.push_back([r]() {
                ssize_t nread = ::pread(r.fd, r.buffer, r.size,
                                        static_cast<off_t>(r.offset));
                if (nread < 0) {
                    return Status::IoError(
                        "NodeLocalAccessor: pread failed, errno=" +
                        std::string(strerror(errno)));
                }
                if (static_cast<size_t>(nread) < r.size) {
                    return Status::IoError(
                        "NodeLocalAccessor: partial read, expected=" +
                        std::to_string(r.size) + " got=" +
                        std::to_string(nread));
                }
                return Status::OK();
            });
        }

        auto task_results = io_pool_->SubmitAndWait(std::move(tasks));
        for (size_t j = 0; j < task_results.size(); ++j) {
            results[valid_indices[j]] = task_results[j];
        }
        return results;
    }

    // Last resort: sequential read (no IO engines initialized)
    for (size_t i = 0; i < requests.size(); ++i) {
        if (!results[i].ok()) continue;
        results[i] = Read(requests[i].store_id, requests[i].offset,
                          requests[i].buffer, requests[i].size);
    }
    return results;
}

void NodeLocalAccessor::Close() {
    if (io_uring_engine_) {
        io_uring_engine_->Close();
        io_uring_engine_.reset();
    }
    if (io_pool_) {
        io_pool_->Stop();
        io_pool_.reset();
    }
    if (fd_cache_) {
        fd_cache_->CloseAll();
    }
}

} // namespace falconkv

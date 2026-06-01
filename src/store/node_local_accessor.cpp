#include "src/store/node_local_accessor.h"

#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <memory>

#include "src/store/fd_cache.h"
#include "src/common/aligned_allocator.h"
#include "src/common/logging.h"

namespace falconkv {

// DirectIO alignment constant
static constexpr size_t kDirectIOAlignment = 512;

static inline size_t AlignUpSize(size_t value, size_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

static inline bool IsAlignedPtr(const void* ptr, size_t alignment) {
    return (reinterpret_cast<uintptr_t>(ptr) & (alignment - 1)) == 0;
}

NodeLocalAccessor::NodeLocalAccessor()
    : fd_cache_(std::make_unique<FdCache>()) {}

NodeLocalAccessor::~NodeLocalAccessor() {
    Close();
}

void NodeLocalAccessor::RegisterStoreFile(uint32_t store_id,
                                           const std::string& data_file) {
    std::lock_guard<std::mutex> lock(mutex_);
    store_files_[store_id] = data_file;
}

int NodeLocalAccessor::GetFd(uint32_t store_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = store_files_.find(store_id);
    if (it == store_files_.end()) {
        return -1;
    }
    return fd_cache_->GetFd(it->second);
}

Status NodeLocalAccessor::Write(uint32_t store_id, uint64_t offset,
                                 const void* data, uint32_t size) {
    int fd = GetFd(store_id);
    if (fd < 0) {
        LOG(ERROR) << "[NodeLocalAccessor] Write: no file for store_id " << store_id;
        return Status::IoError("NodeLocalAccessor: no file for store_id " +
                               std::to_string(store_id));
    }

    size_t aligned_size = AlignUpSize(size, kDirectIOAlignment);

    if (IsAlignedPtr(data, kDirectIOAlignment) && size == aligned_size) {
        ssize_t written = ::pwrite(fd, data, aligned_size,
                                    static_cast<off_t>(offset));
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
    } else {
        void* aligned_buf = AlignedAllocator::Allocate(kDirectIOAlignment,
                                                        aligned_size);
        if (!aligned_buf) {
            LOG(ERROR) << "[NodeLocalAccessor] Write: posix_memalign failed, size="
                       << aligned_size;
            return Status::IoError("NodeLocalAccessor: posix_memalign failed");
        }

        std::memset(aligned_buf, 0, aligned_size);
        std::memcpy(aligned_buf, data, size);

        ssize_t written = ::pwrite(fd, aligned_buf, aligned_size,
                                    static_cast<off_t>(offset));
        AlignedAllocator::Free(aligned_buf);

        if (written < 0) {
            LOG(ERROR) << "[NodeLocalAccessor] Write: pwrite (aligned) failed for store_id "
                       << store_id << ", offset=" << offset << ": " << strerror(errno);
            return Status::IoError("NodeLocalAccessor: pwrite failed for store_id " +
                                   std::to_string(store_id));
        }
        if (static_cast<size_t>(written) < size) {
            LOG(ERROR) << "[NodeLocalAccessor] Write: partial write (aligned) for store_id "
                       << store_id << ", expected=" << size << " written=" << written;
            return Status::IoError("NodeLocalAccessor: partial write for store_id " +
                                   std::to_string(store_id));
        }
    }

    return Status::OK();
}

Status NodeLocalAccessor::Read(uint32_t store_id, uint64_t offset,
                                void* buffer, uint32_t size) {
    int fd = GetFd(store_id);
    if (fd < 0) {
        LOG(ERROR) << "[NodeLocalAccessor] Read: no file for store_id " << store_id;
        return Status::IoError("NodeLocalAccessor: no file for store_id " +
                               std::to_string(store_id));
    }

    size_t aligned_size = AlignUpSize(size, kDirectIOAlignment);

    if (IsAlignedPtr(buffer, kDirectIOAlignment) && size == aligned_size) {
        ssize_t nread = ::pread(fd, buffer, aligned_size,
                                 static_cast<off_t>(offset));
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
    } else {
        void* aligned_buf = AlignedAllocator::Allocate(kDirectIOAlignment,
                                                        aligned_size);
        if (!aligned_buf) {
            LOG(ERROR) << "[NodeLocalAccessor] Read: posix_memalign failed, size="
                       << aligned_size;
            return Status::IoError("NodeLocalAccessor: posix_memalign failed");
        }

        ssize_t nread = ::pread(fd, aligned_buf, aligned_size,
                                 static_cast<off_t>(offset));
        if (nread < 0) {
            AlignedAllocator::Free(aligned_buf);
            LOG(ERROR) << "[NodeLocalAccessor] Read: pread (aligned) failed for store_id "
                       << store_id << ", offset=" << offset << ": " << strerror(errno);
            return Status::IoError("NodeLocalAccessor: pread failed for store_id " +
                                   std::to_string(store_id));
        }

        size_t copy_size = std::min(static_cast<size_t>(nread),
                                     static_cast<size_t>(size));
        std::memcpy(buffer, aligned_buf, copy_size);
        AlignedAllocator::Free(aligned_buf);

        if (copy_size < size) {
            LOG(ERROR) << "[NodeLocalAccessor] Read: partial read (aligned) for store_id "
                       << store_id << ", expected=" << size << " got=" << copy_size;
            return Status::IoError("NodeLocalAccessor: partial read for store_id " +
                                   std::to_string(store_id));
        }
    }

    return Status::OK();
}

void NodeLocalAccessor::Close() {
    if (fd_cache_) {
        fd_cache_->CloseAll();
    }
}

} // namespace falconkv

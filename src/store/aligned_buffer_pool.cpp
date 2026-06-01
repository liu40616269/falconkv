#include "src/store/aligned_buffer_pool.h"

#include "src/common/logging.h"

namespace falconkv {

AlignedBufferPool::AlignedBufferPool(size_t buffer_size, size_t alignment, size_t pool_size)
    : buffer_size_(buffer_size), alignment_(alignment) {
    for (size_t i = 0; i < pool_size; ++i) {
        void* buf = AlignedAllocator::Allocate(alignment_, buffer_size_);
        if (buf) {
            free_buffers_.push(buf);
        } else {
            LOG(ERROR) << "AlignedBufferPool: failed to allocate buffer " << i
                       << "/" << pool_size << " (size=" << buffer_size_
                       << ", alignment=" << alignment_ << ")";
        }
    }
}

AlignedBufferPool::~AlignedBufferPool() {
    std::lock_guard<std::mutex> lock(mutex_);
    while (!free_buffers_.empty()) {
        void* buf = free_buffers_.top();
        free_buffers_.pop();
        AlignedAllocator::Free(buf);
    }
}

void* AlignedBufferPool::Get() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (free_buffers_.empty()) {
        // Dynamically allocate a new buffer if the pool is exhausted.
        void* buf = AlignedAllocator::Allocate(alignment_, buffer_size_);
        return buf;
    }
    void* buf = free_buffers_.top();
    free_buffers_.pop();
    return buf;
}

void AlignedBufferPool::Put(void* buf) {
    if (!buf) return;
    std::lock_guard<std::mutex> lock(mutex_);
    free_buffers_.push(buf);
}

} // namespace falconkv

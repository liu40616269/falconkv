#pragma once

#include <stack>
#include <mutex>
#include <cstddef>
#include "src/common/aligned_allocator.h"

namespace falconkv {

class AlignedBufferPool {
public:
    AlignedBufferPool(size_t buffer_size, size_t alignment, size_t pool_size);
    ~AlignedBufferPool();

    AlignedBufferPool(const AlignedBufferPool&) = delete;
    AlignedBufferPool& operator=(const AlignedBufferPool&) = delete;

    void* Get();
    void Put(void* buf);

    size_t buffer_size() const { return buffer_size_; }

private:
    size_t buffer_size_;
    size_t alignment_;
    std::stack<void*> free_buffers_;
    std::mutex mutex_;
};

} // namespace falconkv

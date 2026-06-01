#pragma once
#include <cstdlib>
#include <cstdint>
#include <cstddef>

namespace falconkv {

class AlignedAllocator {
public:
    static void* Allocate(size_t alignment, size_t size) {
        void* ptr = nullptr;
        int ret = posix_memalign(&ptr, alignment, size);
        if (ret != 0) return nullptr;
        return ptr;
    }

    static void Free(void* ptr) {
        free(ptr);
    }

    static bool IsAligned(const void* ptr, size_t alignment) {
        return (reinterpret_cast<uintptr_t>(ptr) % alignment) == 0;
    }

    static size_t AlignUp(size_t value, size_t alignment) {
        return (value + alignment - 1) & ~(alignment - 1);
    }
};

} // namespace falconkv

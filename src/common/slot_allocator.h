#pragma once
#include <cstdint>
#include <vector>
#include <mutex>

namespace falconkv {

class SlotAllocator {
public:
    // slot_size = 0 means auto-detect from first Alloc() call
    SlotAllocator(uint64_t total_bytes, uint32_t slot_size = 0);
    ~SlotAllocator() = default;

    // Non-copyable
    SlotAllocator(const SlotAllocator&) = delete;
    SlotAllocator& operator=(const SlotAllocator&) = delete;

    /// Allocate one slot. Returns byte offset, or -1 on failure.
    /// If `out_alloc_size` is non-null, receives slot_size_.
    /// When auto-detected (slot_size was 0), size must exactly match the first Alloc's size.
    /// When explicitly configured, size must be <= slot_size_.
    int64_t Alloc(uint32_t size, uint32_t* out_alloc_size = nullptr);

    /// Allocate N slots in one lock acquisition. Returns byte offsets in `offsets`.
    /// Returns the number of slots actually allocated (0 to count).
    /// All slots are the same size (slot_size_). More efficient than calling Alloc() N times.
    uint32_t BatchAlloc(uint32_t size, uint32_t count,
                        std::vector<int64_t>& offsets,
                        uint32_t* out_alloc_size = nullptr);

    void Free(int64_t offset, uint32_t alloc_size);
    double GetUsageRatio() const;
    uint64_t GetTotalBytes() const;
    uint64_t GetUsedBytes() const;
    uint32_t GetPageSize() const;

    /// Returns slot_size_ after initialization, or the requested size before init.
    uint32_t ComputeAllocSize(uint32_t size) const;

private:
    uint32_t slot_size_;
    uint32_t total_slots_;
    uint32_t used_slots_;
    uint64_t total_bytes_;
    uint32_t next_slot_;         // Bump pointer: next virgin slot to allocate
    bool initialized_;           // true once slot_size_ is determined
    bool auto_detected_;         // true = auto-detected from first Alloc (strict size match)
                                 // false = explicitly configured (size <= slot_size_ allowed)
    std::vector<uint32_t> free_stack_;  // LIFO stack of freed slot indices
    mutable std::mutex mutex_;
};

} // namespace falconkv

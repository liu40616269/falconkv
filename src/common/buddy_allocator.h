#pragma once
#include <cstdint>
#include <vector>
#include <list>
#include <mutex>

namespace falconkv {

class BuddyAllocator {
public:
    BuddyAllocator(uint64_t total_bytes, uint32_t page_size);
    ~BuddyAllocator() = default;

    // Non-copyable
    BuddyAllocator(const BuddyAllocator&) = delete;
    BuddyAllocator& operator=(const BuddyAllocator&) = delete;

    /// Allocate space for `size` bytes. Returns byte offset, or -1 on failure.
    /// If `out_alloc_size` is non-null, receives the actual allocated size
    /// (aligned to page_size, rounded up to power-of-two pages).
    int64_t Alloc(uint32_t size, uint32_t* out_alloc_size = nullptr);
    void Free(int64_t offset, uint32_t alloc_size);
    double GetUsageRatio() const;
    uint64_t GetTotalBytes() const;
    uint64_t GetUsedBytes() const;
    uint32_t GetPageSize() const { return page_size_; }

    /// Compute the allocation size for a given data size (aligned to page_size,
    /// rounded up to power-of-two pages). Does not perform allocation.
    uint32_t ComputeAllocSize(uint32_t size) const;

private:
    uint32_t FindFreeOrder(uint32_t target_order);
    void SplitBlock(uint32_t from_order, uint32_t to_order);
    void TryMergeBuddies(uint32_t page_start, uint32_t order);

    uint32_t page_size_;
    uint32_t total_pages_;
    uint32_t used_pages_;
    uint32_t max_order_;
    std::vector<bool> page_bitmap_;              // 1 bit per page: true = used
    std::vector<std::list<uint32_t>> free_list_; // free_list_[order] = list of page-start indices
    mutable std::mutex mutex_;
};

} // namespace falconkv

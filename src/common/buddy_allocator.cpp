#include "src/common/buddy_allocator.h"

#include <cmath>
#include <cassert>
#include <algorithm>

#include "src/common/logging.h"

namespace falconkv {

// ---------- helpers (file-local) ----------

static uint32_t CeilLog2(uint32_t v) {
    if (v <= 1) return 0;
    uint32_t r = 0;
    uint32_t val = v - 1;
    while (val > 0) {
        ++r;
        val >>= 1;
    }
    return r;
}

static uint32_t AlignUp(uint32_t value, uint32_t alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

// ---------- ctor / public ----------

BuddyAllocator::BuddyAllocator(uint64_t total_bytes, uint32_t page_size)
    : page_size_(page_size),
      total_pages_(0),
      used_pages_(0),
      max_order_(0) {
    assert(page_size > 0);

    total_pages_ = static_cast<uint32_t>(total_bytes / page_size);
    if (total_pages_ == 0) total_pages_ = 1;

    max_order_ = CeilLog2(total_pages_);
    if (max_order_ < 1) max_order_ = 1;

    // The free_list_ is indexed by order (0 .. max_order_).
    free_list_.resize(max_order_ + 1);

    // page_bitmap_: one entry per page, initialised to false (free).
    page_bitmap_.assign(total_pages_, false);

    // Initially the whole space is one big free block at max_order_.
    free_list_[max_order_].push_back(0);
}

uint32_t BuddyAllocator::ComputeAllocSize(uint32_t size) const {
    if (size == 0) return page_size_;
    uint32_t aligned = AlignUp(size, page_size_);
    uint32_t pages_needed = aligned / page_size_;
    if (pages_needed == 0) pages_needed = 1;
    uint32_t target_order = CeilLog2(pages_needed);
    return (1u << target_order) * page_size_;
}

int64_t BuddyAllocator::Alloc(uint32_t size, uint32_t* out_alloc_size) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Compute pages needed from actual data size.
    uint32_t aligned = AlignUp(size, page_size_);
    uint32_t pages_needed = aligned / page_size_;
    if (pages_needed == 0) pages_needed = 1;
    uint32_t target_order = CeilLog2(pages_needed);

    // Ensure target_order does not exceed max_order_.
    if (target_order > max_order_) {
        LOG(ERROR) << "[BuddyAllocator] Alloc: size too large, pages_needed="
                   << pages_needed << ", max_order=" << max_order_;
        return -1;
    }

    // Find a free block of suitable order.
    uint32_t found_order = FindFreeOrder(target_order);
    if (found_order > max_order_) {
        LOG(WARNING) << "[BuddyAllocator] Alloc: out of space, used_pages="
                     << used_pages_ << "/" << total_pages_
                     << " (" << (total_pages_ > 0 ? (used_pages_ * 100 / total_pages_) : 0) << "%)";
        return -1;
    }

    // Split down to target_order if necessary.
    if (found_order > target_order) {
        SplitBlock(found_order, target_order);
    }

    // The first entry in free_list_[target_order] is our block.
    assert(!free_list_[target_order].empty());
    uint32_t page_start = free_list_[target_order].front();
    free_list_[target_order].pop_front();

    // Mark pages as used.
    uint32_t block_pages = 1u << target_order;
    for (uint32_t i = page_start; i < page_start + block_pages && i < total_pages_; ++i) {
        page_bitmap_[i] = true;
    }
    used_pages_ += block_pages;

    // Return actual allocated size.
    uint32_t alloc_size = block_pages * page_size_;
    if (out_alloc_size) {
        *out_alloc_size = alloc_size;
    }

    return static_cast<int64_t>(page_start) * page_size_;
}

void BuddyAllocator::Free(int64_t offset, uint32_t alloc_size) {
    if (offset < 0) return;

    std::lock_guard<std::mutex> lock(mutex_);

    uint32_t page_start = static_cast<uint32_t>(offset / page_size_);
    if (page_start >= total_pages_) return;

    // Determine the order from the actual allocation size.
    uint32_t pages = alloc_size / page_size_;
    if (pages == 0) pages = 1;
    uint32_t target_order = CeilLog2(pages);
    if (target_order > max_order_) target_order = max_order_;

    uint32_t block_pages = 1u << target_order;

    // Mark pages as free.
    for (uint32_t i = page_start; i < page_start + block_pages && i < total_pages_; ++i) {
        page_bitmap_[i] = false;
    }
    used_pages_ -= std::min(block_pages, total_pages_ - page_start);

    // Return to free list and try to merge.
    free_list_[target_order].push_back(page_start);
    TryMergeBuddies(page_start, target_order);
}

double BuddyAllocator::GetUsageRatio() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (total_pages_ == 0) return 0.0;
    return static_cast<double>(used_pages_) / static_cast<double>(total_pages_);
}

uint64_t BuddyAllocator::GetTotalBytes() const {
    return static_cast<uint64_t>(total_pages_) * page_size_;
}

uint64_t BuddyAllocator::GetUsedBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<uint64_t>(used_pages_) * page_size_;
}

// ---------- private ----------

uint32_t BuddyAllocator::FindFreeOrder(uint32_t target_order) {
    // Walk from target_order upward until we find a non-empty free list.
    for (uint32_t o = target_order; o <= max_order_; ++o) {
        if (!free_list_[o].empty()) {
            return o;
        }
    }
    return max_order_ + 1; // sentinel: not found
}

void BuddyAllocator::SplitBlock(uint32_t from_order, uint32_t to_order) {
    // Repeatedly split the first block at from_order into two halves until we
    // reach to_order.
    assert(from_order > to_order);

    for (uint32_t o = from_order; o > to_order; --o) {
        assert(!free_list_[o].empty());
        uint32_t page_idx = free_list_[o].front();
        free_list_[o].pop_front();

        uint32_t half_pages = 1u << (o - 1);
        // Left buddy
        free_list_[o - 1].push_back(page_idx);
        // Right buddy
        free_list_[o - 1].push_back(page_idx + half_pages);
    }
}

void BuddyAllocator::TryMergeBuddies(uint32_t page_start, uint32_t order) {
    // Iteratively try to merge with the buddy at the same order.
    uint32_t current = page_start;
    uint32_t cur_order = order;

    while (cur_order < max_order_) {
        uint32_t block_pages = 1u << cur_order;
        uint32_t buddy = current ^ block_pages; // XOR to find buddy

        // Verify buddy is free and not used by scanning the free list.
        if (buddy + block_pages > total_pages_) {
            // Buddy is out of range, cannot merge.
            break;
        }

        // Check if buddy is entirely free (all pages in buddy range are false).
        bool buddy_free = true;
        for (uint32_t i = buddy; i < buddy + block_pages; ++i) {
            if (page_bitmap_[i]) {
                buddy_free = false;
                break;
            }
        }
        if (!buddy_free) break;

        // Try to find and remove buddy from the free list.
        auto& fl = free_list_[cur_order];
        auto it = std::find(fl.begin(), fl.end(), buddy);
        if (it == fl.end()) break; // buddy not in free list (shouldn't happen)

        fl.erase(it);

        // Remove current as well (it was just added).
        auto it2 = std::find(fl.begin(), fl.end(), current);
        if (it2 != fl.end()) {
            fl.erase(it2);
        }

        // Merge into the higher order.
        uint32_t merged_start = std::min(current, buddy);
        cur_order++;
        free_list_[cur_order].push_back(merged_start);
        current = merged_start;
    }
}

} // namespace falconkv

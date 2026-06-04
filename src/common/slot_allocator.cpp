#include "src/common/slot_allocator.h"

#include "src/common/logging.h"

namespace falconkv {

// ---------- ctor ----------

SlotAllocator::SlotAllocator(uint64_t total_bytes, uint32_t slot_size)
    : slot_size_(slot_size),
      total_slots_(0),
      used_slots_(0),
      total_bytes_(total_bytes),
      next_slot_(0),
      initialized_(false),
      auto_detected_(false) {
    if (slot_size_ > 0) {
        // Explicit configuration: compute total_slots immediately, no free_stack pre-fill
        total_slots_ = static_cast<uint32_t>(total_bytes_ / slot_size_);
        if (total_slots_ == 0) total_slots_ = 1;
        total_bytes_ = static_cast<uint64_t>(total_slots_) * slot_size_;
        initialized_ = true;
        auto_detected_ = false;
    }
    // slot_size_ == 0: deferred until first Alloc() call
}

// ---------- public ----------

uint32_t SlotAllocator::ComputeAllocSize(uint32_t size) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) return size;
    return slot_size_;
}

uint32_t SlotAllocator::GetPageSize() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return slot_size_;
}

uint64_t SlotAllocator::GetTotalBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return total_bytes_;
}

int64_t SlotAllocator::Alloc(uint32_t size, uint32_t* out_alloc_size) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Lazy init: auto-detect slot_size from first Alloc's size
    if (!initialized_) {
        if (size == 0) {
            LOG(ERROR) << "[SlotAllocator] Alloc: cannot auto-detect slot_size from size=0";
            return -1;
        }
        slot_size_ = size;
        total_slots_ = static_cast<uint32_t>(total_bytes_ / slot_size_);
        if (total_slots_ == 0) total_slots_ = 1;
        total_bytes_ = static_cast<uint64_t>(total_slots_) * slot_size_;
        next_slot_ = 0;
        initialized_ = true;
        auto_detected_ = true;
        LOG(INFO) << "[SlotAllocator] Auto-detected slot_size=" << slot_size_
                  << ", total_slots=" << total_slots_;
    }

    // Size check
    if (auto_detected_) {
        if (size != slot_size_) {
            LOG(ERROR) << "[SlotAllocator] Alloc: requested size " << size
                       << " != slot_size " << slot_size_
                       << " (auto-detected mode requires exact match)";
            return -1;
        }
    } else {
        if (size > slot_size_) {
            LOG(ERROR) << "[SlotAllocator] Alloc: requested size " << size
                       << " exceeds slot_size " << slot_size_;
            return -1;
        }
    }

    // Try free_stack first (reuse freed slots), then bump from virgin region
    uint32_t slot_index;
    if (!free_stack_.empty()) {
        slot_index = free_stack_.back();
        free_stack_.pop_back();
    } else if (next_slot_ < total_slots_) {
        slot_index = next_slot_++;
    } else {
        LOG(WARNING) << "[SlotAllocator] Alloc: out of space, used_slots="
                     << used_slots_ << "/" << total_slots_;
        return -1;
    }

    used_slots_++;
    if (out_alloc_size) {
        *out_alloc_size = slot_size_;
    }
    return static_cast<int64_t>(slot_index) * slot_size_;
}

uint32_t SlotAllocator::BatchAlloc(uint32_t size, uint32_t count,
                                    std::vector<int64_t>& offsets,
                                    uint32_t* out_alloc_size) {
    if (count == 0) return 0;

    std::lock_guard<std::mutex> lock(mutex_);

    // Lazy init (same as Alloc)
    if (!initialized_) {
        if (size == 0) return 0;
        slot_size_ = size;
        total_slots_ = static_cast<uint32_t>(total_bytes_ / slot_size_);
        if (total_slots_ == 0) total_slots_ = 1;
        total_bytes_ = static_cast<uint64_t>(total_slots_) * slot_size_;
        next_slot_ = 0;
        initialized_ = true;
        auto_detected_ = true;
        LOG(INFO) << "[SlotAllocator] Auto-detected slot_size=" << slot_size_
                  << ", total_slots=" << total_slots_;
    }

    // Size check
    if (auto_detected_) {
        if (size != slot_size_) return 0;
    } else {
        if (size > slot_size_) return 0;
    }

    offsets.clear();
    offsets.reserve(count);

    // First drain free_stack
    while (!free_stack_.empty() && offsets.size() < count) {
        uint32_t slot_index = free_stack_.back();
        free_stack_.pop_back();
        offsets.push_back(static_cast<int64_t>(slot_index) * slot_size_);
    }

    // Then bump from virgin region
    while (next_slot_ < total_slots_ && offsets.size() < count) {
        offsets.push_back(static_cast<int64_t>(next_slot_++) * slot_size_);
    }

    uint32_t allocated = static_cast<uint32_t>(offsets.size());
    used_slots_ += allocated;
    if (out_alloc_size) {
        *out_alloc_size = slot_size_;
    }
    return allocated;
}

void SlotAllocator::Free(int64_t offset, uint32_t alloc_size) {
    if (offset < 0) return;

    std::lock_guard<std::mutex> lock(mutex_);

    if (!initialized_) return;

    // Verify alignment
    if (static_cast<uint64_t>(offset) % slot_size_ != 0) {
        LOG(ERROR) << "[SlotAllocator] Free: offset " << offset
                   << " is not aligned to slot_size " << slot_size_;
        return;
    }

    uint32_t slot_index = static_cast<uint32_t>(offset / slot_size_);
    if (slot_index >= total_slots_) {
        LOG(ERROR) << "[SlotAllocator] Free: slot_index " << slot_index
                   << " out of range (total_slots=" << total_slots_ << ")";
        return;
    }

    free_stack_.push_back(slot_index);
    used_slots_--;
}

double SlotAllocator::GetUsageRatio() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_ || total_slots_ == 0) return 0.0;
    return static_cast<double>(used_slots_) / static_cast<double>(total_slots_);
}

uint64_t SlotAllocator::GetUsedBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) return 0;
    return static_cast<uint64_t>(used_slots_) * slot_size_;
}

} // namespace falconkv

#include "src/store/pending_evict_queue.h"
#include "src/common/buddy_allocator.h"
#include "src/common/time_util.h"

#include <chrono>

namespace falconkv {

PendingEvictQueue::PendingEvictQueue(uint64_t grace_period_ms,
                                     BuddyAllocator* allocator)
    : grace_period_ms_(grace_period_ms), allocator_(allocator) {}

PendingEvictQueue::~PendingEvictQueue() {
    Stop();
}

void PendingEvictQueue::Enqueue(const std::string& key, uint64_t offset) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.push_back({key, offset, GetWallTimeMs()});
}

void PendingEvictQueue::Start() {
    if (running_.exchange(true)) {
        return; // already running
    }
    thread_ = std::thread(&PendingEvictQueue::EvictLoop, this);
}

void PendingEvictQueue::Stop() {
    if (!running_.exchange(false)) {
        return;
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    FlushAll();
}

size_t PendingEvictQueue::Size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

void PendingEvictQueue::EvictLoop() {
    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        uint64_t now = GetWallTimeMs();
        std::vector<EvictEntry> to_free;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            size_t keep_from = 0;
            for (size_t i = 0; i < entries_.size(); ++i) {
                if (now - entries_[i].enqueue_time_ms >= grace_period_ms_) {
                    to_free.push_back(std::move(entries_[i]));
                } else {
                    entries_[keep_from++] = std::move(entries_[i]);
                }
            }
            entries_.resize(keep_from);
        }

        // Free space outside the lock.
        for (const auto& e : to_free) {
            allocator_->FreeChunk(static_cast<int64_t>(e.offset));
        }
    }
}

void PendingEvictQueue::FlushAll() {
    std::vector<EvictEntry> remaining;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        remaining = std::move(entries_);
        entries_.clear();
    }
    for (const auto& e : remaining) {
        allocator_->FreeChunk(static_cast<int64_t>(e.offset));
    }
}

} // namespace falconkv

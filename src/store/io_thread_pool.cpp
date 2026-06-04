#include "src/store/io_thread_pool.h"
#include "src/common/logging.h"

namespace falconkv {

IOThreadPool::IOThreadPool(uint32_t num_threads)
    : num_threads_(num_threads > 0 ? num_threads : 4) {}

IOThreadPool::~IOThreadPool() {
    Stop();
}

void IOThreadPool::Start() {
    if (running_.exchange(true)) {
        return; // already started
    }
    workers_.reserve(num_threads_);
    for (uint32_t i = 0; i < num_threads_; ++i) {
        workers_.emplace_back(&IOThreadPool::WorkerLoop, this);
    }
    LOG(INFO) << "[IOThreadPool] Started with " << num_threads_ << " threads";
}

void IOThreadPool::Stop() {
    if (!running_.exchange(false)) {
        return; // already stopped
    }
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        queue_cv_.notify_all();
    }
    for (auto& w : workers_) {
        if (w.joinable()) {
            w.join();
        }
    }
    workers_.clear();
    // Drain remaining tasks (shouldn't happen in normal use).
    std::lock_guard<std::mutex> lock(queue_mutex_);
    while (!task_queue_.empty()) {
        task_queue_.pop();
    }
    LOG(INFO) << "[IOThreadPool] Stopped";
}

void IOThreadPool::WorkerLoop() {
    while (running_.load()) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait(lock, [this] {
                return !task_queue_.empty() || !running_.load();
            });
            if (!running_.load() && task_queue_.empty()) {
                return;
            }
            if (task_queue_.empty()) {
                continue;
            }
            task = std::move(task_queue_.front());
            task_queue_.pop();
        }
        task();
    }
}

std::vector<Status> IOThreadPool::SubmitAndWait(
    std::vector<std::function<Status()>> tasks) {
    size_t n = tasks.size();
    std::vector<Status> results(n);
    if (n == 0) return results;

    std::vector<std::future<Status>> futures;
    futures.reserve(n);

    for (size_t i = 0; i < n; ++i) {
        auto task = std::make_shared<std::packaged_task<Status()>>(
            std::move(tasks[i]));
        futures.push_back(task->get_future());
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            task_queue_.push([task]() { (*task)(); });
        }
        queue_cv_.notify_one();
    }

    for (size_t i = 0; i < n; ++i) {
        results[i] = futures[i].get();
    }
    return results;
}

uint32_t IOThreadPool::num_threads() const {
    return num_threads_;
}

} // namespace falconkv

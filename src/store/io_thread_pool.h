#pragma once

#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <atomic>
#include <future>
#include <cstdint>

#include "src/common/status.h"

namespace falconkv {

class IOThreadPool {
public:
    explicit IOThreadPool(uint32_t num_threads);
    ~IOThreadPool();

    IOThreadPool(const IOThreadPool&) = delete;
    IOThreadPool& operator=(const IOThreadPool&) = delete;

    void Start();
    void Stop();

    // Submit a batch of tasks, block until all complete, return each task's Status.
    std::vector<Status> SubmitAndWait(std::vector<std::function<Status()>> tasks);

    uint32_t num_threads() const;

private:
    void WorkerLoop();

    uint32_t num_threads_;
    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> task_queue_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::atomic<bool> running_{false};
};

} // namespace falconkv

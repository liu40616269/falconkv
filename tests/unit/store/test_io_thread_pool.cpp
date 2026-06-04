#include <gtest/gtest.h>
#include <vector>
#include <atomic>
#include <chrono>
#include <thread>

#include "src/store/io_thread_pool.h"
#include "src/common/status.h"

using namespace falconkv;

// ---------------------------------------------------------------------------
// Basic lifecycle
// ---------------------------------------------------------------------------
TEST(IOThreadPoolTest, StartStop) {
    IOThreadPool pool(4);
    pool.Start();
    EXPECT_EQ(pool.num_threads(), 4u);
    pool.Stop();
}

TEST(IOThreadPoolTest, DoubleStartStop) {
    IOThreadPool pool(2);
    pool.Start();
    pool.Start(); // second start should be no-op
    pool.Stop();
    pool.Stop(); // second stop should be no-op
}

TEST(IOThreadPoolTest, StopInDestructor) {
    {
        IOThreadPool pool(2);
        pool.Start();
    }
    // destructor should not hang
}

// ---------------------------------------------------------------------------
// SubmitAndWait
// ---------------------------------------------------------------------------
TEST(IOThreadPoolTest, SubmitAndWaitEmpty) {
    IOThreadPool pool(2);
    pool.Start();
    auto results = pool.SubmitAndWait({});
    EXPECT_EQ(results.size(), 0u);
    pool.Stop();
}

TEST(IOThreadPoolTest, SubmitAndWaitAllOK) {
    IOThreadPool pool(4);
    pool.Start();

    std::vector<std::function<Status()>> tasks;
    for (int i = 0; i < 10; ++i) {
        tasks.push_back([]() { return Status::OK(); });
    }
    auto results = pool.SubmitAndWait(std::move(tasks));
    EXPECT_EQ(results.size(), 10u);
    for (const auto& s : results) {
        EXPECT_TRUE(s.ok());
    }
    pool.Stop();
}

TEST(IOThreadPoolTest, SubmitAndWaitMixedResults) {
    IOThreadPool pool(4);
    pool.Start();

    std::vector<std::function<Status()>> tasks;
    tasks.push_back([]() { return Status::OK(); });
    tasks.push_back([]() { return Status::IoError("test error"); });
    tasks.push_back([]() { return Status::OK(); });
    tasks.push_back([]() { return Status::NotFound("not found"); });

    auto results = pool.SubmitAndWait(std::move(tasks));
    EXPECT_EQ(results.size(), 4u);
    EXPECT_TRUE(results[0].ok());
    EXPECT_TRUE(results[1].IsIOError());
    EXPECT_TRUE(results[2].ok());
    EXPECT_TRUE(results[3].IsNotFound());
    pool.Stop();
}

// ---------------------------------------------------------------------------
// Concurrent execution
// ---------------------------------------------------------------------------
TEST(IOThreadPoolTest, ConcurrentExecution) {
    IOThreadPool pool(4);
    pool.Start();

    std::atomic<int> counter{0};
    std::vector<std::function<Status()>> tasks;
    for (int i = 0; i < 100; ++i) {
        tasks.push_back([&counter]() {
            counter.fetch_add(1);
            return Status::OK();
        });
    }
    auto results = pool.SubmitAndWait(std::move(tasks));
    EXPECT_EQ(results.size(), 100u);
    EXPECT_EQ(counter.load(), 100);
    pool.Stop();
}

TEST(IOThreadPoolTest, TasksRunInParallel) {
    // Verify that tasks actually run in parallel by having multiple tasks
    // sleep for a while; the total wall time should be much less than
    // num_tasks * sleep_time.
    IOThreadPool pool(4);
    pool.Start();

    auto start = std::chrono::steady_clock::now();
    std::vector<std::function<Status()>> tasks;
    for (int i = 0; i < 4; ++i) {
        tasks.push_back([]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            return Status::OK();
        });
    }
    auto results = pool.SubmitAndWait(std::move(tasks));
    auto elapsed = std::chrono::steady_clock::now() - start;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

    // With 4 threads, 4 tasks of 100ms each should complete in ~100ms,
    // definitely less than 400ms.
    EXPECT_LT(ms, 350);
    EXPECT_EQ(results.size(), 4u);
    pool.Stop();
}

// ---------------------------------------------------------------------------
// Zero thread handling
// ---------------------------------------------------------------------------
TEST(IOThreadPoolTest, ZeroThreadsUsesDefault) {
    IOThreadPool pool(0);
    EXPECT_GE(pool.num_threads(), 1u);
    pool.Start();
    pool.Stop();
}

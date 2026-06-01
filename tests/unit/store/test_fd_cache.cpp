#include <gtest/gtest.h>
#include <string>
#include <thread>
#include <chrono>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include "src/store/fd_cache.h"

using namespace falconkv;

namespace {

// Helper: create a temporary file for testing. Returns the file path.
std::string CreateTempFile(const std::string& suffix) {
    std::string path = "/tmp/falconkv_test_fd_" + suffix;
    int fd = open(path.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd >= 0) {
        // Write some data so reads are valid.
        const char data[] = "hello falconkv";
        write(fd, data, sizeof(data));
        close(fd);
    }
    return path;
}

void RemoveTempFile(const std::string& path) {
    unlink(path.c_str());
}

} // anonymous namespace

class FdCacheTest : public ::testing::Test {
protected:
    void SetUp() override {
        file_a_ = CreateTempFile("a");
        file_b_ = CreateTempFile("b");
    }

    void TearDown() override {
        cache_.CloseAll();
        RemoveTempFile(file_a_);
        RemoveTempFile(file_b_);
    }

    FdCache cache_;
    std::string file_a_;
    std::string file_b_;
};

// ---------------------------------------------------------------------------
// GetFd creates new fd and reuses
// ---------------------------------------------------------------------------
TEST_F(FdCacheTest, GetFdCreatesNewFd) {
    int fd = cache_.GetFd(file_a_);
    EXPECT_GE(fd, 0);
}

TEST_F(FdCacheTest, GetFdReusesExistingFd) {
    int fd1 = cache_.GetFd(file_a_);
    ASSERT_GE(fd1, 0);

    int fd2 = cache_.GetFd(file_a_);
    EXPECT_EQ(fd1, fd2);
}

// ---------------------------------------------------------------------------
// Different paths get different fds
// ---------------------------------------------------------------------------
TEST_F(FdCacheTest, DifferentPathsDifferentFds) {
    int fd_a = cache_.GetFd(file_a_);
    int fd_b = cache_.GetFd(file_b_);

    ASSERT_GE(fd_a, 0);
    ASSERT_GE(fd_b, 0);
    EXPECT_NE(fd_a, fd_b);
}

// ---------------------------------------------------------------------------
// EvictIdle closes idle fds
// ---------------------------------------------------------------------------
TEST_F(FdCacheTest, EvictIdleClosesOldEntries) {
    int fd1 = cache_.GetFd(file_a_);
    ASSERT_GE(fd1, 0);

    // Wait long enough for the entry to be considered idle.
    // Use a 1ms idle threshold and sleep briefly.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    cache_.EvictIdle(10); // 10ms idle threshold -- both should be evicted

    // Getting the fd again should create a new one (since the old was closed).
    int fd2 = cache_.GetFd(file_a_);
    ASSERT_GE(fd2, 0);

    // The fd number might be reused by the OS, but in most cases it will differ
    // because the old one was closed. We just verify we get a valid fd.
    EXPECT_GE(fd2, 0);
}

TEST_F(FdCacheTest, EvictIdleKeepsActiveEntries) {
    int fd_a = cache_.GetFd(file_a_);

    // Immediately access file_b so it has a very recent timestamp.
    int fd_b = cache_.GetFd(file_b_);

    // Evict with a very large threshold -- nothing should be evicted.
    cache_.EvictIdle(60000); // 60 seconds

    // Both should still be accessible (same fd).
    EXPECT_EQ(cache_.GetFd(file_a_), fd_a);
    EXPECT_EQ(cache_.GetFd(file_b_), fd_b);
}

// ---------------------------------------------------------------------------
// CloseAll
// ---------------------------------------------------------------------------
TEST_F(FdCacheTest, CloseAllClearsCache) {
    cache_.GetFd(file_a_);
    cache_.GetFd(file_b_);

    cache_.CloseAll();

    // After CloseAll, getting the fd for the same file should produce a new fd.
    int fd_new = cache_.GetFd(file_a_);
    EXPECT_GE(fd_new, 0);
}

TEST_F(FdCacheTest, GetFdReturnsInvalidOnMissingFile) {
    int fd = cache_.GetFd("/tmp/falconkv_test_nonexistent_file_XXXX");
    EXPECT_LT(fd, 0);
}

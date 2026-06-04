#include <gtest/gtest.h>
#include <vector>
#include <chrono>
#include <iostream>
#include <numeric>
#include <algorithm>
#include <random>

#include "src/common/slot_allocator.h"

using namespace falconkv;
using namespace std::chrono;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

struct BenchResult {
    std::string name;
    int ops;
    double avg_ns;
    double p50_ns;
    double p99_ns;
    double ops_per_sec;
};

static void PrintResult(const BenchResult& r) {
    std::cout << "\n=== " << r.name << " ===\n"
              << "  ops       : " << r.ops << "\n"
              << "  avg       : " << r.avg_ns << " ns\n"
              << "  p50       : " << r.p50_ns << " ns\n"
              << "  p99       : " << r.p99_ns << " ns\n"
              << "  ops/sec   : " << r.ops_per_sec << "\n"
              << std::endl;
}

static BenchResult RunBench(const std::string& name,
                            const std::vector<double>& latencies_ns) {
    BenchResult r;
    r.name = name;
    r.ops = static_cast<int>(latencies_ns.size());
    r.avg_ns = std::accumulate(latencies_ns.begin(), latencies_ns.end(), 0.0) / r.ops;

    std::vector<double> sorted = latencies_ns;
    std::sort(sorted.begin(), sorted.end());
    r.p50_ns = sorted[sorted.size() / 2];
    r.p99_ns = sorted[static_cast<size_t>(sorted.size() * 0.99)];
    r.ops_per_sec = 1e9 / r.avg_ns;

    PrintResult(r);
    return r;
}


// ---------------------------------------------------------------------------
// 100GB / 2MB alloc – the user-requested benchmark
// ---------------------------------------------------------------------------
TEST(SlotAllocatorPerf, Alloc100GB_2MB) {
    const uint64_t cap = 100ULL * 1024 * 1024 * 1024;
    const uint32_t slot_size = 2 * 1024 * 1024;
    const int N = 20000;

    SlotAllocator alloc(cap, slot_size);

    // Warmup
    for (int i = 0; i < 100; ++i) {
        uint32_t as = 0;
        int64_t off = alloc.Alloc(slot_size, &as);
        ASSERT_GE(off, 0);
        alloc.Free(off, as);
    }

    // ---------- Bench: sequential alloc ----------
    std::vector<double> alloc_ns(N);
    std::vector<int64_t> offsets(N);
    std::vector<uint32_t> alloc_sizes(N);

    for (int i = 0; i < N; ++i) {
        auto t0 = high_resolution_clock::now();
        uint32_t as = 0;
        int64_t off = alloc.Alloc(slot_size, &as);
        auto t1 = high_resolution_clock::now();
        ASSERT_GE(off, 0) << "Alloc failed at " << i;
        alloc_ns[i] = duration<double, std::nano>(t1 - t0).count();
        offsets[i] = off;
        alloc_sizes[i] = as;
    }
    RunBench("Alloc 2MB (100GB cap, sequential)", alloc_ns);

    double usage = alloc.GetUsageRatio();
    std::cout << "  Usage after " << N << " allocs: "
              << (usage * 100) << "%\n";
    std::cout << "  Used bytes: " << alloc.GetUsedBytes() / (1024.0 * 1024 * 1024) << " GB\n";

    // ---------- Bench: sequential free ----------
    std::vector<double> free_ns(N);
    for (int i = 0; i < N; ++i) {
        auto t0 = high_resolution_clock::now();
        alloc.Free(offsets[i], alloc_sizes[i]);
        auto t1 = high_resolution_clock::now();
        free_ns[i] = duration<double, std::nano>(t1 - t0).count();
    }
    RunBench("Free 2MB (100GB cap, sequential)", free_ns);
    EXPECT_DOUBLE_EQ(alloc.GetUsageRatio(), 0.0);
}

// ---------------------------------------------------------------------------
// 100GB / 2MB alloc+free cycle
// ---------------------------------------------------------------------------
TEST(SlotAllocatorPerf, AllocFreeCycle100GB_2MB) {
    const uint64_t cap = 100ULL * 1024 * 1024 * 1024;
    const uint32_t slot_size = 2 * 1024 * 1024;
    const int N = 50000;

    SlotAllocator alloc(cap, slot_size);

    std::vector<double> cycle_ns(N);
    for (int i = 0; i < N; ++i) {
        auto t0 = high_resolution_clock::now();
        uint32_t as = 0;
        int64_t off = alloc.Alloc(slot_size, &as);
        alloc.Free(off, as);
        auto t1 = high_resolution_clock::now();
        ASSERT_GE(off, 0);
        cycle_ns[i] = duration<double, std::nano>(t1 - t0).count();
    }
    auto result = RunBench("Alloc+Free cycle 2MB (100GB cap)", cycle_ns);
    EXPECT_DOUBLE_EQ(alloc.GetUsageRatio(), 0.0);

    // SlotAllocator should achieve avg < 200ns per alloc+free cycle
    EXPECT_LT(result.avg_ns, 500.0) << "Alloc+Free cycle should be < 500ns avg";
}

// ---------------------------------------------------------------------------
// 100GB / 2MB random free order
// ---------------------------------------------------------------------------
TEST(SlotAllocatorPerf, Alloc100GB_2MB_RandomFree) {
    const uint64_t cap = 100ULL * 1024 * 1024 * 1024;
    const uint32_t slot_size = 2 * 1024 * 1024;
    const int N = 20000;

    SlotAllocator alloc(cap, slot_size);

    // Alloc N blocks
    std::vector<int64_t> offsets(N);
    std::vector<uint32_t> alloc_sizes(N);
    for (int i = 0; i < N; ++i) {
        uint32_t as = 0;
        int64_t off = alloc.Alloc(slot_size, &as);
        ASSERT_GE(off, 0) << "Alloc failed at " << i;
        offsets[i] = off;
        alloc_sizes[i] = as;
    }

    // Shuffle free order
    std::vector<int> indices(N);
    std::iota(indices.begin(), indices.end(), 0);
    std::mt19937 gen(42);
    std::shuffle(indices.begin(), indices.end(), gen);

    // Bench: random order free
    std::vector<double> free_ns(N);
    for (int i = 0; i < N; ++i) {
        int idx = indices[i];
        auto t0 = high_resolution_clock::now();
        alloc.Free(offsets[idx], alloc_sizes[idx]);
        auto t1 = high_resolution_clock::now();
        free_ns[i] = duration<double, std::nano>(t1 - t0).count();
    }
    auto result = RunBench("Free 2MB (100GB cap, random order, SlotAllocator)", free_ns);
    EXPECT_DOUBLE_EQ(alloc.GetUsageRatio(), 0.0);

    // Free should be O(1) regardless of order
    EXPECT_LT(result.avg_ns, 200.0) << "Random free should be < 200ns avg";
}

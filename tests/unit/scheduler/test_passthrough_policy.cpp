#include <gtest/gtest.h>
#include <string>
#include <vector>

#include "src/scheduler/passthrough_policy.h"

using namespace falconkv;

// ---------------------------------------------------------------------------
// Decide returns status=0 (passthrough)
// ---------------------------------------------------------------------------
TEST(PassthroughPolicy, DecideReturnsStatusZero) {
    PassthroughPolicy policy;

    IORequestData request;
    request.client_id = 1;
    request.io_channel = 0;
    request.store_id = 1;
    request.io_size = 4096;

    IOResponseData response = policy.Decide(request);

    EXPECT_EQ(response.status, 0);
}

TEST(PassthroughPolicy, DecideReturnsNonZeroPermittedTs) {
    PassthroughPolicy policy;

    IORequestData request;
    IOResponseData response = policy.Decide(request);

    EXPECT_GT(response.permitted_ts_ns, 0u);
}

// ---------------------------------------------------------------------------
// Ticket is monotonically increasing
// ---------------------------------------------------------------------------
TEST(PassthroughPolicy, TicketMonotonicallyIncreasing) {
    PassthroughPolicy policy;

    IORequestData request;

    std::vector<uint64_t> tickets;
    for (int i = 0; i < 100; ++i) {
        IOResponseData response = policy.Decide(request);
        tickets.push_back(response.ticket);
    }

    for (size_t i = 1; i < tickets.size(); ++i) {
        EXPECT_GT(tickets[i], tickets[i - 1])
            << "Ticket " << i << " (" << tickets[i]
            << ") should be > ticket " << (i - 1) << " (" << tickets[i - 1] << ")";
    }
}

// ---------------------------------------------------------------------------
// OnIOComplete is no-op
// ---------------------------------------------------------------------------
TEST(PassthroughPolicy, OnIOCompleteIsNoop) {
    PassthroughPolicy policy;

    IOCompletionData report;
    report.client_id = 1;
    report.ticket = 42;
    report.io_start_ts_ns = 1000;
    report.io_done_ts_ns = 2000;
    report.io_size = 4096;
    report.io_channel = 0;
    report.io_status = 0;

    // Calling OnIOComplete should not affect subsequent decisions.
    IOResponseData before = policy.Decide(IORequestData{});
    policy.OnIOComplete(report);
    IOResponseData after = policy.Decide(IORequestData{});

    EXPECT_EQ(before.status, 0);
    EXPECT_EQ(after.status, 0);
    EXPECT_GT(after.ticket, before.ticket); // Still monotonically increasing.
}

// ---------------------------------------------------------------------------
// Name
// ---------------------------------------------------------------------------
TEST(PassthroughPolicy, NameReturnsPassthrough) {
    PassthroughPolicy policy;
    EXPECT_EQ(policy.Name(), "passthrough");
}

// ---------------------------------------------------------------------------
// Interface via IOSchedulePolicy pointer
// ---------------------------------------------------------------------------
TEST(PassthroughPolicy, PolymorphicAccess) {
    std::unique_ptr<IOSchedulePolicy> policy = std::make_unique<PassthroughPolicy>();

    IORequestData request;
    request.io_size = 8192;

    IOResponseData response = policy->Decide(request);
    EXPECT_EQ(response.status, 0);
    EXPECT_EQ(policy->Name(), "passthrough");
}

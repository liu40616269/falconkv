#include "src/scheduler/passthrough_policy.h"

#include <chrono>

namespace falconkv {

namespace {

uint64_t NowNanos() {
    auto tp = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(tp).count());
}

}  // namespace

// ---------------------------------------------------------------------------
// PassthroughPolicy
// ---------------------------------------------------------------------------

IOResponseData PassthroughPolicy::Decide(const IORequestData& /*request*/) {
    IOResponseData resp;
    resp.status = 0;  // success
    resp.permitted_ts_ns = NowNanos();
    resp.ticket = ticket_counter_.fetch_add(1, std::memory_order_relaxed);
    return resp;
}

void PassthroughPolicy::OnIOComplete(const IOCompletionData& /*report*/) {
    // Passthrough policy does not track completions.
}

}  // namespace falconkv

#pragma once
#include <atomic>
#include <string>
#include <memory>
#include "src/common/status.h"

namespace falconkv {

// Plain-struct equivalents of the proto messages, to avoid a proto dependency
// in the public headers.  Values are kept as simple integers / strings so that
// every compilation unit can work with them without including generated code.

struct IORequestData {
    uint32_t client_id = 0;
    int io_channel = 0;       // IOChannel enum value
    uint32_t store_id = 0;
    uint64_t io_size = 0;
    uint32_t priority = 0;
    uint64_t request_ts_ns = 0;
    std::string remote_node_addr;
};

struct IOResponseData {
    int32_t status = 0;
    uint64_t permitted_ts_ns = 0;
    uint64_t ticket = 0;
};

struct IOCompletionData {
    uint32_t client_id = 0;
    uint64_t ticket = 0;
    uint64_t io_start_ts_ns = 0;
    uint64_t io_done_ts_ns = 0;
    uint64_t io_size = 0;
    int io_channel = 0;
    int32_t io_status = 0;
    uint32_t store_id = 0;
    std::string remote_node_addr;
};

// ---------------------------------------------------------------------------
// IOSchedulePolicy -- abstract interface for every scheduling policy.
// ---------------------------------------------------------------------------
class IOSchedulePolicy {
public:
    virtual ~IOSchedulePolicy() = default;

    /// Called by the scheduler for every incoming IO request.  Returns an
    /// IOResponseData that tells the caller whether (and when) the IO is
    /// permitted.
    virtual IOResponseData Decide(const IORequestData& request) = 0;

    /// Called when an IO completes so the policy can update internal state.
    virtual void OnIOComplete(const IOCompletionData& report) = 0;

    /// Human-readable name (e.g. "passthrough", "token-bucket", ...).
    virtual std::string Name() const = 0;
};

// ---------------------------------------------------------------------------
// PassthroughPolicy -- trivially admits every request immediately.
// ---------------------------------------------------------------------------
class PassthroughPolicy : public IOSchedulePolicy {
public:
    IOResponseData Decide(const IORequestData& request) override;
    void OnIOComplete(const IOCompletionData& report) override;
    std::string Name() const override { return "passthrough"; }

private:
    std::atomic<uint64_t> ticket_counter_{0};
};

}  // namespace falconkv

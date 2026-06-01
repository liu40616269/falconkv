#pragma once
#include <functional>
#include <chrono>
#include "src/common/status.h"

namespace falconkv {

class RetryPolicy {
public:
    RetryPolicy(int max_retry = 3, int64_t base_delay_ms = 100);
    Status ExecuteWithRetry(std::function<Status()> fn);

private:
    bool IsRetriable(const Status& s);
    int max_retry_;
    std::chrono::milliseconds base_delay_;
};

} // namespace falconkv

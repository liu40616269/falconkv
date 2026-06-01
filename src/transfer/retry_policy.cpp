#include "src/transfer/retry_policy.h"

#include <thread>
#include <cstdlib>

namespace falconkv {

RetryPolicy::RetryPolicy(int max_retry, int64_t base_delay_ms)
    : max_retry_(max_retry),
      base_delay_(base_delay_ms) {}

bool RetryPolicy::IsRetriable(const Status& s) {
    return s.IsNetworkError() || s.IsIOError() || s.code() == Status::kMaxRetryExceeded;
}

Status RetryPolicy::ExecuteWithRetry(std::function<Status()> fn) {
    Status last_status;
    for (int attempt = 0; attempt <= max_retry_; ++attempt) {
        last_status = fn();
        if (last_status.ok()) {
            return last_status;
        }

        if (!IsRetriable(last_status)) {
            return last_status;
        }

        if (attempt < max_retry_) {
            // Exponential backoff: base_delay * 2^attempt
            auto delay = base_delay_ * (1 << attempt);
            std::this_thread::sleep_for(delay);
        }
    }

    return Status::MaxRetryExceeded();
}

} // namespace falconkv

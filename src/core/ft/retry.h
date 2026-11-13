#pragma once

#include <chrono>
#include <functional>
#include <thread>

namespace vnxfer {

struct RetryPolicy {
    int attempts = 3;
    int delay_ms = 500;
    bool backoff = true;
};

// Runs fn(attempt) until it succeeds or the attempt budget is exhausted.
// fn is expected to open its own connection, so a retry starts from scratch.
template <typename Fn>
void with_retry(const RetryPolicy& policy, Fn&& fn) {
    int attempt = 0;
    for (;;) {
        try {
            fn(attempt);
            return;
        } catch (const std::exception&) {
            ++attempt;
            if (attempt >= policy.attempts) throw;
            int delay = policy.delay_ms;
            if (policy.backoff) {
                for (int i = 1; i < attempt && delay < 30000; ++i) delay *= 2;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(delay));
        }
    }
}

}  // namespace vnxfer

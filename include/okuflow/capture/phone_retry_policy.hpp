#pragma once
#include <algorithm>
#include <cstdint>
#include <string>

namespace okuflow {
// Per USB transport: transient failures back off; new transport/readiness resets it.
class PhoneRetryPolicy {
public:
    void Observe(std::uint64_t transport, bool ready, const std::string& problem) {
        if (transport != transport_ || (ready && !ready_) || problem != problem_) {
            failures_ = 0;
            nextAttemptMs_ = 0;
        }
        transport_ = transport;
        ready_ = ready;
        problem_ = problem;
    }
    bool CanAttempt(std::int64_t nowMs) const { return nowMs >= nextAttemptMs_; }
    void Failed(std::int64_t nowMs, bool busy = false) {
        constexpr std::int64_t delays[] = {5000, 10000, 20000, 60000};
        auto delay = delays[std::min(failures_, 3u)];
        if (busy) delay = std::max<std::int64_t>(delay, 30000);
        ++failures_;
        nextAttemptMs_ = nowMs + delay;
    }
    void Succeeded() { failures_ = 0; nextAttemptMs_ = 0; }
private:
    std::uint64_t transport_{};
    bool ready_{};
    std::string problem_;
    unsigned failures_{};
    std::int64_t nextAttemptMs_{};
};
} // namespace okuflow

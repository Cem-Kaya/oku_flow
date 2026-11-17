#pragma once
#include <algorithm>
#include <cstdint>
#include <string>

namespace okuflow {
// Per USB transport: transient failures back off; new transport/readiness resets it.
// Camera-start retries for a ready phone. The ceiling (10 s) keeps a lecture
// recoverable; events (arrival, resume, manual pick) call Reset.
class PhoneRetryPolicy {
public:
    void Observe(std::uint64_t transport, bool ready, const std::string& problem,
                 std::uint64_t serverEpoch = 0) {
        if (transport != transport_ || serverEpoch != serverEpoch_ || (ready && !ready_) || problem != problem_) {
            failures_ = 0;
            nextAttemptMs_ = 0;
        }
        transport_ = transport;
        serverEpoch_ = serverEpoch;
        ready_ = ready;
        problem_ = problem;
    }
    bool CanAttempt(std::int64_t nowMs) const { return nowMs >= nextAttemptMs_; }
    void Failed(std::int64_t nowMs, bool busy = false) {
        constexpr std::int64_t delays[] = {1000, 2000, 3000, 5000, 10000};
        auto delay = delays[std::min(failures_, 4u)];
        // The phone holds a busy/failed problem for 10 s; retrying sooner only repeats it.
        if (busy) delay = std::max<std::int64_t>(delay, 5000);
        ++failures_;
        nextAttemptMs_ = nowMs + delay;
    }
    void Succeeded() { failures_ = 0; nextAttemptMs_ = 0; }
    // An event (arrival, removal, resume, manual pick) starts a fresh ladder.
    void Reset() { failures_ = 0; nextAttemptMs_ = 0; }
private:
    std::uint64_t transport_{};
    std::uint64_t serverEpoch_{};
    bool ready_{};
    std::string problem_;
    unsigned failures_{};
    std::int64_t nextAttemptMs_{};
};
} // namespace okuflow

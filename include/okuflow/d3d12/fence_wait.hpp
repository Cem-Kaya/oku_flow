#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

namespace okuflow {

enum class FenceWaitResult { Completed, TimedOut, DeviceLost, WaitFailed };
enum class FenceEventResult { Signaled, TimedOut, Failed };
enum class FenceProgressResult { Healthy, TimedOut, DeviceLost };

// Polling only: observe actual submitted/completed fence values, never swap-chain
// latency readiness. New submissions cannot renew a stalled completion deadline.
// A timeout is sticky and never grants permission to retire an allocation.
class FenceProgressWatchdog {
public:
    FenceProgressResult Observe(std::uint64_t submitted, std::uint64_t completed,
                            std::uint64_t nowMs) noexcept {
        if (failed_ != FenceProgressResult::Healthy) return failed_;
        if (completed == std::numeric_limits<std::uint64_t>::max())
            return failed_ = FenceProgressResult::DeviceLost;
        if (completed >= submitted) {
            pending_ = false;
            completed_ = completed;
            return FenceProgressResult::Healthy;
        }
        if (!pending_ || completed > completed_) {
            pending_ = true;
            completed_ = completed;
            progressAtMs_ = nowMs;
        } else if (nowMs - progressAtMs_ >= 1000) {
            return failed_ = FenceProgressResult::TimedOut;
        }
        return FenceProgressResult::Healthy;
    }
private:
    bool pending_{false};
    std::uint64_t completed_{}, progressAtMs_{};
    FenceProgressResult failed_{FenceProgressResult::Healthy};
};

// Event wakeups are hints only. Completion must be confirmed by the fence;
// D3D12's UINT64_MAX device-removal sentinel is never successful completion.
// Injected clock/event access makes deadline and removal cases testable without
// a GPU or an intentionally wedged driver.
template<class Completed, class DeviceLost, class Wait, class Clock>
FenceWaitResult WaitForFenceDeadline(std::uint64_t target,
                                     std::uint64_t timeoutMs,
                                     Completed completed,
                                     DeviceLost deviceLost,
                                     Wait wait,
                                     Clock clock)
{
    const auto start = clock();
    for (;;) {
        const auto value = completed();
        if (value == std::numeric_limits<std::uint64_t>::max() || deviceLost())
            return FenceWaitResult::DeviceLost;
        if (value >= target) return FenceWaitResult::Completed;
        const auto elapsed = clock() - start;
        if (elapsed >= timeoutMs) return FenceWaitResult::TimedOut;
        const auto slice = std::min<std::uint64_t>(50, timeoutMs - elapsed);
        if (wait(slice) == FenceEventResult::Failed) return FenceWaitResult::WaitFailed;
    }
}

} // namespace okuflow

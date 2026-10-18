#pragma once

#include <chrono>

namespace okuflow {

// One host deadline includes adb/listener negotiation, phone mode attempts and decode.
class PhoneStartupBudget {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    static constexpr auto Timeout = std::chrono::seconds(12);
    static constexpr auto DecodeReserve = std::chrono::seconds(1);

    explicit PhoneStartupBudget(TimePoint now) : deadline_(now + Timeout) {}
    TimePoint Deadline() const noexcept { return deadline_; }
    int RemainingCameraMs(TimePoint now) const noexcept {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline_ - now - DecodeReserve).count();
        return static_cast<int>(remaining > 0 ? remaining : 0);
    }

private:
    TimePoint deadline_;
};

} // namespace okuflow

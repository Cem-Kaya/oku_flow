#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>

namespace okuflow::detail {

// Long-GOP software preroll can be healthy for more than two seconds. Only
// strictly advancing decoded media PTS earns another short progress window;
// empty/repeated/backward samples and late native returns cannot renew it.
class VideoPlaybackSeekBudget {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr auto kProgressWindow = std::chrono::seconds(2);
    static constexpr auto kRequestLimit = std::chrono::seconds(60);

    explicit VideoPlaybackSeekBudget(Clock::time_point started)
        : started_(started), progressed_(started) {}
    Clock::time_point Deadline() const {
        return std::min(started_ + kRequestLimit, progressed_ + kProgressWindow);
    }
    bool Expired(Clock::time_point now) const { return now >= Deadline(); }
    bool Observe(std::int64_t pts100ns, Clock::time_point now) {
        if (pts100ns < 0 || pts100ns > std::numeric_limits<std::int64_t>::max()/100 ||
            pts100ns <= furthestPts_ || Expired(now)) return false;
        furthestPts_ = pts100ns;
        progressed_ = now;
        return true;
    }

private:
    Clock::time_point started_, progressed_;
    std::int64_t furthestPts_{-1};
};

} // namespace okuflow::detail

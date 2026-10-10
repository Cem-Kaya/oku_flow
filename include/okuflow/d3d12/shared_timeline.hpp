#pragma once
#include <algorithm>
#include <cstdint>
#include <limits>

namespace okuflow {
// Used only by the UI-thread presenter. Reserved holes are never wait targets.
class SharedFenceTimeline {
public:
    std::uint64_t ReserveExternal() noexcept {
        if (pending_ || value_ >= std::numeric_limits<std::uint64_t>::max() - 1) return 0;
        return pending_ = ++value_;
    }
    bool CommitExternal(std::uint64_t value) noexcept {
        if (!value || pending_ != value) return false;
        external_ = value; pending_ = 0; return true;
    }
    bool CancelExternal(std::uint64_t value) noexcept {
        if (!value || pending_ != value) return false;
        pending_ = 0; return true;
    }
    template<class Wait> bool QueueDependency(Wait wait) {
        if (pending_) return false;
        if (external_ <= waited_) return true;
        if (!wait(external_)) return false;
        waited_ = external_; return true;
    }
    std::uint64_t ReserveGraphics() noexcept {
        if (pending_ || external_ > waited_ ||
            value_ >= std::numeric_limits<std::uint64_t>::max() - 1) return 0;
        return ++value_;
    }
    void GraphicsSubmitted(std::uint64_t value) noexcept { graphics_ = value; }
    std::uint64_t LastGraphics() const noexcept { return graphics_; }
    std::uint64_t LastExternal() const noexcept { return external_; }
    std::uint64_t LastReserved() const noexcept { return value_; }
private:
    std::uint64_t value_{}, pending_{}, external_{}, waited_{}, graphics_{};
};
} // namespace okuflow

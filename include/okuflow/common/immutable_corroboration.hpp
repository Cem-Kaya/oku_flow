#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>

namespace okuflow {
class VideoPlayback;
struct UctControllerTestAccess;
namespace uct {

inline constexpr std::int64_t kOrdinaryNeighbourWindow100ns = 2'500'000;
inline constexpr std::int64_t kSavedNeighbourCeiling100ns = 25'000'000;

// Duration is native sample metadata, never elapsed wall time or an estimate
// from two unrelated frames. Zero and unrepresentable durations are unknown.
inline std::int64_t ImmutableNeighbourWindow100ns(bool savedFile, std::int64_t duration100ns)
{
    if (!savedFile || duration100ns <= 0 || duration100ns > std::numeric_limits<std::int64_t>::max() / 100)
        return kOrdinaryNeighbourWindow100ns;
    return std::clamp(duration100ns, kOrdinaryNeighbourWindow100ns, kSavedNeighbourCeiling100ns);
}

// Only VideoPlayback can issue an expanded descriptor, after checking the
// exact native frame/control block against its private issuance lease. Holding
// this descriptor retains metadata, not another decoded image or a player.
class ImmutableCorroborationPolicy {
    friend class ::okuflow::VideoPlayback;
    friend struct ::okuflow::UctControllerTestAccess;
    std::shared_ptr<const void> issuance_;
    std::uint64_t selectedFrame_{};
    std::int64_t selectedPts100ns_{}, duration100ns_{};
    ImmutableCorroborationPolicy(std::shared_ptr<const void> issuance, std::uint64_t frame,
                                std::int64_t pts100ns, std::int64_t duration100ns)
        : issuance_(std::move(issuance)), selectedFrame_(frame),
          selectedPts100ns_(pts100ns), duration100ns_(duration100ns) {}
public:
    ImmutableCorroborationPolicy() = default;
    bool operator==(const ImmutableCorroborationPolicy& other) const noexcept {
        return issuance_ == other.issuance_ && selectedFrame_ == other.selectedFrame_ &&
            selectedPts100ns_ == other.selectedPts100ns_ && duration100ns_ == other.duration100ns_;
    }
    bool operator!=(const ImmutableCorroborationPolicy& other) const noexcept { return !(*this == other); }
    bool MatchesSelected(std::uint64_t frame, std::int64_t ptsMs) const noexcept {
        return !issuance_ || (selectedPts100ns_ >= 0 && selectedFrame_ == frame &&
            selectedFrame_ == static_cast<std::uint64_t>(selectedPts100ns_) &&
            selectedPts100ns_ / 10'000 == ptsMs);
    }
    bool Allows(std::uint64_t selectedFrame, std::int64_t selectedMs,
                std::uint64_t neighbourFrame, std::int64_t neighbourMs) const noexcept {
        if (!MatchesSelected(selectedFrame, selectedMs) || selectedFrame == neighbourFrame ||
            selectedMs < 0 || neighbourMs < 0) return false;
        // An issued saved-file sample ID is its exact native 100-ns PTS.
        // Keep that precision through the proof path, including boundaries.
        if (issuance_) {
            if (neighbourFrame > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() / 100) ||
                neighbourFrame / 10'000 != static_cast<std::uint64_t>(neighbourMs)) return false;
            const auto pts = static_cast<std::int64_t>(neighbourFrame);
            return pts > selectedPts100ns_
                ? pts - selectedPts100ns_ <= ImmutableNeighbourWindow100ns(true, duration100ns_)
                : selectedPts100ns_ - pts <= kOrdinaryNeighbourWindow100ns;
        }
        return (neighbourMs >= selectedMs ? neighbourMs - selectedMs : selectedMs - neighbourMs) <= 250;
    }
};

} // namespace uct
} // namespace okuflow

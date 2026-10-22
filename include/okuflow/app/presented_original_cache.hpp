#pragma once

#ifdef _WIN32

#include "okuflow/app/recording_manager.hpp"
#include "okuflow/common/view_transform.hpp"

#include <memory>
#include <optional>

namespace okuflow {

// Two logical slots only: the displayed clean Original frame and its newest
// processed successor awaiting a presentation receipt. GPU allocations share
// the Original producer lease, shared with recorder/recap admission. Compressed
// rolling history and the exact processed-scene Freeze lease have other owners.
class PresentedOriginalCache {
public:
    static constexpr std::size_t kByteLimit = 96u * 1024 * 1024;
    struct ViewRecipe {
        bool canReproduceCleanView{true};
        bool blackWhite{};
        float blackWhiteThreshold{0.5f};
        int colorTransform{};
        std::uint64_t colorLutGeneration{};
        float contrast{1.0f}, brightness{};
        bool operator==(const ViewRecipe&) const = default;
    };
    struct Frame {
        std::shared_ptr<const CapturedFrame> original;
        QString recordingSessionId;
        std::uint64_t cameraEpoch{};
        int rotationQuarterTurns{};
        ViewTransform transform;
        ViewRecipe recipe;
        bool canReproduceCleanView{};
    };

    bool Stage(const CapturedFrame& original, const QString& recordingSessionId,
               std::uint64_t cameraEpoch, int rotationQuarterTurns)
    {
        return Stage(original, recordingSessionId, cameraEpoch, rotationQuarterTurns, ViewRecipe{});
    }

    bool Stage(const CapturedFrame& original, const QString& recordingSessionId,
               std::uint64_t cameraEpoch, int rotationQuarterTurns, const ViewRecipe& recipe)
    {
        candidate_.reset();
        if (recordingSessionId.isEmpty() || !original.IsValid()) return false;
        if (displayed_ && (displayed_->recordingSessionId != recordingSessionId ||
                           displayed_->cameraEpoch != cameraEpoch)) displayed_.reset();
        const auto bytes = Bytes(original);
        const auto displayedBytes = displayed_ ? Bytes(*displayed_->original) : 0;
        if (bytes == 0 || bytes > kByteLimit || displayedBytes > kByteLimit - bytes) return false;
        candidate_ = Frame{std::make_shared<const CapturedFrame>(original),
            recordingSessionId, cameraEpoch, rotationQuarterTurns, {}, recipe,
            recipe.canReproduceCleanView};
        return true;
    }

    // Only a successful present may replace the held authority. A deferred
    // successor leaves displayed_ intact. A successful unretained successor
    // revokes it, so Pause can never silently fall back to an older image.
    void Presented(std::uint64_t cameraEpoch, std::uint64_t sequence,
                   const ViewTransform& transform, bool canReproducePresentation = true)
    {
        const auto matches = [cameraEpoch, sequence](const std::optional<Frame>& frame) {
            return frame && frame->cameraEpoch == cameraEpoch &&
                frame->original->identity.sequenceNumber == sequence;
        };
        if (matches(candidate_)) {
            displayed_ = std::move(candidate_);
            candidate_.reset();
        } else if (!matches(displayed_)) {
            displayed_.reset();
        }
        if (displayed_) {
            displayed_->transform = transform;
            displayed_->canReproduceCleanView =
                displayed_->recipe.canReproduceCleanView && canReproducePresentation;
        }
    }

    const std::optional<Frame>& Displayed() const noexcept { return displayed_; }
    std::size_t RetainedBytes() const noexcept
    {
        return (candidate_ ? Bytes(*candidate_->original) : 0) +
               (displayed_ ? Bytes(*displayed_->original) : 0);
    }
    void Reset() { candidate_.reset(); displayed_.reset(); }

private:
    static std::size_t Bytes(const CapturedFrame& frame) noexcept
    {
        if (frame.width == 0 || frame.height == 0 ||
            frame.width > kByteLimit / 4u / frame.height) return 0;
        return static_cast<std::size_t>(frame.width) * frame.height * 4u;
    }
    std::optional<Frame> candidate_, displayed_;
};

} // namespace okuflow

#endif

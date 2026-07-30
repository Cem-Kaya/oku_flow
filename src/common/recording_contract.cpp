#include "openzoom/common/recording_contract.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace openzoom {

namespace {

std::int64_t RoundedRatio(std::uint64_t value,
                          std::uint64_t numerator,
                          std::uint64_t denominator)
{
    if (denominator == 0) {
        return 0;
    }
    const long double scaled =
        static_cast<long double>(value) *
        static_cast<long double>(numerator) /
        static_cast<long double>(denominator);
    return static_cast<std::int64_t>(std::llround(scaled));
}

} // namespace

void RecordingTimeline::Reset(std::uint32_t frameRateNumerator,
                              std::uint32_t frameRateDenominator)
{
    frameRateNumerator_ =
        frameRateNumerator == 0 ? 30u : frameRateNumerator;
    frameRateDenominator_ =
        frameRateDenominator == 0 ? 1u : frameRateDenominator;
    sourceOrigin100ns_ = -1;
    lastTimestamp100ns_ = -1;
    fallbackFrameIndex_ = 0;
    unknownTimestampCount_ = 0;
}

std::int64_t RecordingTimeline::FallbackTimestamp(
    std::uint64_t frameIndex) const
{
    return RoundedRatio(
        frameIndex,
        static_cast<std::uint64_t>(kMediaFoundationTicksPerSecond) *
            frameRateDenominator_,
        frameRateNumerator_);
}

std::int64_t RecordingTimeline::NominalDuration100ns() const
{
    return std::max<std::int64_t>(1, FallbackTimestamp(1));
}

std::int64_t RecordingTimeline::MapTimestamp(
    std::int64_t captureTimestamp100ns)
{
    std::int64_t mapped = 0;
    if (lastTimestamp100ns_ < 0) {
        if (captureTimestamp100ns >= 0) {
            sourceOrigin100ns_ = captureTimestamp100ns;
        } else {
            ++unknownTimestampCount_;
        }
        lastTimestamp100ns_ = 0;
        fallbackFrameIndex_ = 1;
        return 0;
    }

    if (captureTimestamp100ns >= 0) {
        if (sourceOrigin100ns_ < 0) {
            // Align the first trustworthy camera timestamp with the next
            // fallback position so changing from unknown to known remains
            // continuous.
            sourceOrigin100ns_ =
                captureTimestamp100ns - FallbackTimestamp(fallbackFrameIndex_);
        }
        mapped = captureTimestamp100ns - sourceOrigin100ns_;
    } else {
        ++unknownTimestampCount_;
        mapped = FallbackTimestamp(fallbackFrameIndex_);
    }

    if (mapped <= lastTimestamp100ns_) {
        mapped = lastTimestamp100ns_ + NominalDuration100ns();
    }
    lastTimestamp100ns_ = mapped;
    ++fallbackFrameIndex_;
    return mapped;
}

RecordingCanvasSize ResolveRecordingCanvas(RecordingCanvasMode mode,
                                           std::uint32_t sourceWidth,
                                           std::uint32_t sourceHeight)
{
    if (sourceWidth == 0 || sourceHeight == 0) {
        return {};
    }
    if (mode == RecordingCanvasMode::Source) {
        return {sourceWidth, sourceHeight};
    }

    RecordingCanvasSize landscape{};
    switch (mode) {
    case RecordingCanvasMode::Nhd360:
        landscape = {640, 360};
        break;
    case RecordingCanvasMode::Sd480:
        landscape = {854, 480};
        break;
    case RecordingCanvasMode::Hd720:
        landscape = {1280, 720};
        break;
    case RecordingCanvasMode::FullHd1080:
        landscape = {1920, 1080};
        break;
    case RecordingCanvasMode::QuadHd1440:
        landscape = {2560, 1440};
        break;
    case RecordingCanvasMode::UltraHd2160:
        landscape = {3840, 2160};
        break;
    case RecordingCanvasMode::Source:
        break;
    }
    if (sourceHeight > sourceWidth) {
        std::swap(landscape.width, landscape.height);
    }
    return landscape;
}

bool ResampleRecordingCanvas(const std::uint8_t* source,
                             std::uint32_t sourceWidth,
                             std::uint32_t sourceHeight,
                             const RecordingViewTransform& transform,
                             std::uint32_t targetWidth,
                             std::uint32_t targetHeight,
                             std::vector<std::uint8_t>& destination)
{
    if (!source || sourceWidth == 0 || sourceHeight == 0 ||
        targetWidth == 0 || targetHeight == 0 || !transform.valid ||
        transform.sourceWidth <= 0.0f || transform.sourceHeight <= 0.0f ||
        transform.targetWidth <= 0.0f || transform.targetHeight <= 0.0f) {
        destination.clear();
        return false;
    }
    if (static_cast<std::size_t>(targetWidth) >
        std::numeric_limits<std::size_t>::max() /
            (static_cast<std::size_t>(targetHeight) * 4u)) {
        destination.clear();
        return false;
    }

    destination.assign(
        static_cast<std::size_t>(targetWidth) * targetHeight * 4u, 0u);

    const int activeLeft = std::clamp(
        static_cast<int>(std::lround(transform.targetX * targetWidth)),
        0,
        static_cast<int>(targetWidth));
    const int activeTop = std::clamp(
        static_cast<int>(std::lround(transform.targetY * targetHeight)),
        0,
        static_cast<int>(targetHeight));
    const int activeRight = std::clamp(
        static_cast<int>(std::lround(
            (transform.targetX + transform.targetWidth) * targetWidth)),
        activeLeft,
        static_cast<int>(targetWidth));
    const int activeBottom = std::clamp(
        static_cast<int>(std::lround(
            (transform.targetY + transform.targetHeight) * targetHeight)),
        activeTop,
        static_cast<int>(targetHeight));
    const int activeWidth = activeRight - activeLeft;
    const int activeHeight = activeBottom - activeTop;
    if (activeWidth <= 0 || activeHeight <= 0) {
        return true;
    }

    for (int y = 0; y < activeHeight; ++y) {
        const float v =
            transform.sourceY +
            (static_cast<float>(y) + 0.5f) /
                static_cast<float>(activeHeight) *
                transform.sourceHeight;
        const float sourceSampleY = std::clamp(
            v * static_cast<float>(sourceHeight) - 0.5f,
            0.0f,
            static_cast<float>(sourceHeight - 1));
        const std::uint32_t sourceY0 =
            static_cast<std::uint32_t>(std::floor(sourceSampleY));
        const std::uint32_t sourceY1 =
            std::min(sourceHeight - 1, sourceY0 + 1);
        const float fractionY =
            sourceSampleY - static_cast<float>(sourceY0);
        for (int x = 0; x < activeWidth; ++x) {
            const float u =
                transform.sourceX +
                (static_cast<float>(x) + 0.5f) /
                    static_cast<float>(activeWidth) *
                    transform.sourceWidth;
            const float sourceSampleX = std::clamp(
                u * static_cast<float>(sourceWidth) - 0.5f,
                0.0f,
                static_cast<float>(sourceWidth - 1));
            const std::uint32_t sourceX0 =
                static_cast<std::uint32_t>(std::floor(sourceSampleX));
            const std::uint32_t sourceX1 =
                std::min(sourceWidth - 1, sourceX0 + 1);
            const float fractionX =
                sourceSampleX - static_cast<float>(sourceX0);
            const std::uint8_t* source00 =
                source +
                (static_cast<std::size_t>(sourceY0) * sourceWidth +
                 sourceX0) *
                    4u;
            const std::uint8_t* source10 =
                source +
                (static_cast<std::size_t>(sourceY0) * sourceWidth +
                 sourceX1) *
                    4u;
            const std::uint8_t* source01 =
                source +
                (static_cast<std::size_t>(sourceY1) * sourceWidth +
                 sourceX0) *
                    4u;
            const std::uint8_t* source11 =
                source +
                (static_cast<std::size_t>(sourceY1) * sourceWidth +
                 sourceX1) *
                    4u;
            std::uint8_t* destinationPixel =
                destination.data() +
                (static_cast<std::size_t>(activeTop + y) * targetWidth +
                 activeLeft + x) *
                    4u;
            for (int channel = 0; channel < 4; ++channel) {
                const float top =
                    static_cast<float>(source00[channel]) +
                    (static_cast<float>(source10[channel]) -
                     static_cast<float>(source00[channel])) *
                        fractionX;
                const float bottom =
                    static_cast<float>(source01[channel]) +
                    (static_cast<float>(source11[channel]) -
                     static_cast<float>(source01[channel])) *
                        fractionX;
                destinationPixel[channel] =
                    static_cast<std::uint8_t>(std::lround(
                        top + (bottom - top) * fractionY));
            }
        }
    }
    return true;
}

bool IsValidRecordingStateTransition(RecordingState from, RecordingState to)
{
    if (from == to) {
        return true;
    }
    switch (from) {
    case RecordingState::Idle:
        return to == RecordingState::Starting;
    case RecordingState::Starting:
        return to == RecordingState::Recording ||
               to == RecordingState::Stopping ||
               to == RecordingState::Failed;
    case RecordingState::Recording:
        return to == RecordingState::Stopping ||
               to == RecordingState::Finalizing ||
               to == RecordingState::Failed;
    case RecordingState::Stopping:
        return to == RecordingState::Finalizing ||
               to == RecordingState::Completed ||
               to == RecordingState::Failed;
    case RecordingState::Finalizing:
        return to == RecordingState::Completed ||
               to == RecordingState::Failed;
    case RecordingState::Completed:
    case RecordingState::Failed:
        return to == RecordingState::Starting ||
               to == RecordingState::Idle;
    }
    return false;
}

RecordingCompletionOutcome ClassifyRecordingCompletion(
    bool requestedSuccess,
    bool sessionHadOutput,
    bool finalizationSucceeded,
    bool priorFinalizationFailure)
{
    if (!sessionHadOutput) {
        return RecordingCompletionOutcome::FailedBeforeFirstFrame;
    }
    if (requestedSuccess && finalizationSucceeded &&
        !priorFinalizationFailure) {
        return RecordingCompletionOutcome::Saved;
    }
    return RecordingCompletionOutcome::FailedTruncated;
}

} // namespace openzoom

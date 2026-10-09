#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace okuflow {

constexpr std::int64_t kMediaFoundationTicksPerSecond = 10'000'000;

struct RecordingFrameIdentity {
    std::int64_t captureTimestamp100ns{-1};
    std::int64_t captureClock100ns{-1};
    std::uint64_t sequenceNumber{0};
    std::uint32_t frameRateNumerator{0};
    std::uint32_t frameRateDenominator{0};
};

// Converts camera timestamps into a monotonic, zero-based Media Foundation
// timeline. Unknown timestamps advance by the exact negotiated frame-rate
// ratio rather than a rounded integer FPS.
class RecordingTimeline {
public:
    void Reset(std::uint32_t frameRateNumerator,
               std::uint32_t frameRateDenominator);
    std::int64_t MapTimestamp(std::int64_t captureTimestamp100ns);

    std::int64_t NominalDuration100ns() const;
    std::int64_t LastTimestamp100ns() const { return lastTimestamp100ns_; }
    std::uint64_t UnknownTimestampCount() const {
        return unknownTimestampCount_;
    }

private:
    std::int64_t FallbackTimestamp(std::uint64_t frameIndex) const;

    std::uint32_t frameRateNumerator_{30};
    std::uint32_t frameRateDenominator_{1};
    std::int64_t sourceOrigin100ns_{-1};
    std::int64_t lastTimestamp100ns_{-1};
    std::uint64_t fallbackFrameIndex_{0};
    std::uint64_t unknownTimestampCount_{0};
};

enum class RecordingCanvasMode {
    Source = 0,
    FullHd1080 = 1,
    QuadHd1440 = 2,
    UltraHd2160 = 3,
    Hd720 = 4,
    Sd480 = 5,
    Nhd360 = 6,
};

struct RecordingCanvasSize {
    std::uint32_t width{0};
    std::uint32_t height{0};
};

RecordingCanvasSize ResolveRecordingCanvas(RecordingCanvasMode mode,
                                           std::uint32_t sourceWidth,
                                           std::uint32_t sourceHeight);

struct RecordingViewTransform {
    float sourceX{0.0f};
    float sourceY{0.0f};
    float sourceWidth{1.0f};
    float sourceHeight{1.0f};
    float targetX{0.0f};
    float targetY{0.0f};
    float targetWidth{1.0f};
    float targetHeight{1.0f};
    bool valid{false};
};

// Resamples a tightly packed BGRA scene into the fixed recording canvas.
// Source and target rectangles are normalized. Pixels outside the active
// target rectangle are black, matching the viewport Fit policy.
bool ResampleRecordingCanvas(const std::uint8_t* source,
                             std::uint32_t sourceWidth,
                             std::uint32_t sourceHeight,
                             const RecordingViewTransform& transform,
                             std::uint32_t targetWidth,
                             std::uint32_t targetHeight,
                             std::vector<std::uint8_t>& destination);

struct RecordingDropCounts {
    std::uint64_t captureSlotOverwrite{0};
    std::uint64_t readbackRingBusy{0};
    std::uint64_t recordingPoolBusy{0};
    std::uint64_t pairingMiss{0};
    std::uint64_t queueOverflow{0};
    std::uint64_t encoderReject{0};
    std::uint64_t unknownTimestamp{0};

    std::uint64_t Total() const {
        return captureSlotOverwrite + readbackRingBusy +
               recordingPoolBusy + pairingMiss + queueOverflow +
               encoderReject;
    }
};

enum class RecordingState {
    Idle,
    Starting,
    Recording,
    Stopping,
    Finalizing,
    Completed,
    Failed,
};

bool IsValidRecordingStateTransition(RecordingState from, RecordingState to);

enum class RecordingCompletionOutcome {
    Saved,
    FailedBeforeFirstFrame,
    FailedTruncated,
};

// Centralizes the terminal decision used by RecordingManager. In particular,
// no path with a failed finalization may be classified as Saved.
RecordingCompletionOutcome ClassifyRecordingCompletion(
    bool requestedSuccess,
    bool sessionHadOutput,
    bool finalizationSucceeded,
    bool priorFinalizationFailure);

} // namespace okuflow

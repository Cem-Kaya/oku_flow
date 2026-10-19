#pragma once
#include "okuflow/common/uct_device_image.hpp"

namespace okuflow::uct
{
// Host image of the device BumpHoldProvenance record (cuda_kernels.hpp); the
// CUDA surface checks that both layouts match. Copied with a capture.
struct HoldProvenanceRecord
{
    std::uint64_t displayedFrame;
    std::uint64_t heldFrame;
    std::uint32_t state; // 0 live, 1 held, 2 crossfade, 3 unknown
    std::uint32_t heldValid;
    float blendAlpha;
    std::uint32_t reserved;
};
static_assert(sizeof(HoldProvenanceRecord) == 32, "Extra Stable record layout");
inline HoldProvenance DecodeHoldProvenance(const HoldProvenanceRecord &record)
{
    switch (record.state)
    {
    case 0:
        return {HoldDisplay::Live, record.displayedFrame, 1.0f};
    case 1:
        if (record.heldValid != 0 && record.displayedFrame == record.heldFrame)
            return {HoldDisplay::Held, record.heldFrame, 0.0f};
        break;
    case 2:
        if (record.blendAlpha >= 0.0f && record.blendAlpha <= 1.0f)
            return {HoldDisplay::Crossfade, 0, record.blendAlpha};
        break;
    default:
        break;
    }
    return {HoldDisplay::Unknown, 0, 0.0f};
}
// Owner-thread policy: clean resource pressure retries after a bounded pause;
// an unprovable copy completion stays quarantined until an explicit reset.
class AnalysisCaptureHealth
{
  public:
    bool CanAttempt(std::int64_t nowMs) const
    {
        return !quarantined_ && nowMs >= retryAtMs_;
    }
    bool Failed() const
    {
        return quarantined_ || failures_ >= 3;
    }
    void Failure(std::int64_t nowMs, bool unknownCompletion)
    {
        ++failures_;
        quarantined_ |= unknownCompletion;
        retryAtMs_ = nowMs + 250;
    }
    void Success()
    {
        if (quarantined_)
            return;
        failures_ = 0;
        retryAtMs_ = 0;
    }

  private:
    std::int64_t retryAtMs_{};
    int failures_{};
    bool quarantined_{};
};
enum class AnalysisAllocation
{
    Automatic,
    Legacy
};
class AnalysisPool
{
  public:
    explicit AnalysisPool(AnalysisAllocation allocation = AnalysisAllocation::Automatic);
    ~AnalysisPool();
    bool HasFailure() const;
    // Stream-ordered copy, two retained slots; busy means skip. Legacy-driver
    // allocation is rare (new/grown slots) and may synchronize the device.
    // `holdProvenance` is an optional device HoldProvenanceRecord written on
    // the same stream for this exact frame. Its 32 bytes are copied to the
    // slot's pinned record before the ready event; a worker reads them later.
    SharedDeviceImage Capture(const void *bgra, std::size_t pitch, int width, int height,
                              void *stream, std::uint64_t epoch, std::uint64_t frame,
                              std::uint64_t processingGeneration,
                              const void *holdProvenance = nullptr);
    // Atomic paired capture on the same stream: `ocr` is the temporally
    // smoothed neutral image, `validation` the single exact current frame.
    // Both copies share one slot and one stamp; if either storage is busy,
    // fails, or has unknown completion the whole pair is skipped or
    // quarantined and no half-pair is published. `frame` must be the raw
    // current frame (== latest contributing frame of the blend).
    SharedDeviceImage CapturePair(const void *ocr, std::size_t ocrPitch,
                                  const void *validation, std::size_t validationPitch,
                                  int width, int height, void *stream,
                                  std::uint64_t epoch, std::uint64_t frame,
                                  std::uint64_t processingGeneration,
                                  std::uint64_t historyEpoch, std::uint64_t seedFrame,
                                  std::uint64_t blendFrameCount, float temporalAlpha);
    // Explicit desktop-free fault seam (fixtures only). The next storage
    // creation fails as clean allocation pressure, or the next queued copy
    // behaves as if its completion event could not be recorded.
    enum class InjectedFault
    {
        None,
        CleanAllocation,
        UnknownCompletion
    };
    void InjectFaultForTesting(InjectedFault fault);

  private:
    SharedDeviceImage CaptureInternal(const void *ocr, std::size_t ocrPitch,
                                      const void *validation, std::size_t validationPitch,
                                      int width, int height, void *streamHandle,
                                      std::uint64_t epoch, std::uint64_t frame,
                                      std::uint64_t generation, std::uint64_t historyEpoch,
                                      std::uint64_t seedFrame, std::uint64_t blendFrameCount,
                                      float temporalAlpha, const void *holdProvenance);
    struct State;
    std::shared_ptr<State> state_;
};
} // namespace okuflow::uct

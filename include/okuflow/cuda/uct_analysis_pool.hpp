#pragma once
#include "okuflow/common/uct_device_image.hpp"

namespace okuflow::uct
{
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
    SharedDeviceImage Capture(const void *bgra, std::size_t pitch, int width, int height,
                              void *stream, std::uint64_t epoch, std::uint64_t frame,
                              std::uint64_t processingGeneration);

  private:
    struct State;
    std::shared_ptr<State> state_;
};
} // namespace okuflow::uct

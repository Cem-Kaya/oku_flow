#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <tuple>

namespace okuflow {

struct CudaSurfaceConfiguration {
    const void* device{};
    const void* fence{};
    std::uint64_t cameraSession{};
    unsigned int width{};
    unsigned int height{};
    unsigned int superResWidth{};
    unsigned int superResHeight{};

    bool operator==(const CudaSurfaceConfiguration& other) const {
        return std::tie(device, fence, cameraSession, width, height,
                        superResWidth, superResHeight) ==
               std::tie(other.device, other.fence, other.cameraSession,
                        other.width, other.height,
                        other.superResWidth, other.superResHeight);
    }
};

// UI-thread policy shared by raw and converted input paths. The clock is an
// argument so retry timing is testable without CUDA, a device, or sleeping.
class CudaSurfaceRetry {
public:
    using Clock = std::chrono::steady_clock;

    bool ShouldAttempt(const CudaSurfaceConfiguration& configuration,
                       Clock::time_point now) {
        if (!configuration_ || !(*configuration_ == configuration)) {
            configuration_ = configuration;
            RecordSuccess();
        }
        return !retryAt_ || now >= *retryAt_;
    }

    void RecordFailure(Clock::time_point now) {
        retryAt_ = now + std::chrono::seconds(nextDelaySeconds_);
        nextDelaySeconds_ = nextDelaySeconds_ >= 16
                                ? 30 : nextDelaySeconds_ * 2;
    }

    void RecordSuccess() {
        retryAt_.reset();
        nextDelaySeconds_ = 1;
    }

private:
    std::optional<CudaSurfaceConfiguration> configuration_;
    std::optional<Clock::time_point> retryAt_;
    int nextDelaySeconds_{1};
};

} // namespace okuflow

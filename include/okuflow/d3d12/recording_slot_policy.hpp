#pragma once
#include "okuflow/d3d12/frame_readiness.hpp"
namespace okuflow {
constexpr std::uint64_t kRecordingProducerPending = std::numeric_limits<std::uint64_t>::max();
template<class Completed, class DeviceLost>
FrameReadiness PollRecordingSlot(bool inUse, std::uint64_t producer, Completed completed, DeviceLost deviceLost) {
    const auto value = completed();
    if (value == kRecordingProducerPending || deviceLost()) return FrameReadiness::DeviceLost;
    if (inUse || producer == kRecordingProducerPending || value < producer) return FrameReadiness::Busy;
    return FrameReadiness::Ready;
}
} // namespace okuflow

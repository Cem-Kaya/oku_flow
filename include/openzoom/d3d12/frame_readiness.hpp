#pragma once

#include "openzoom/d3d12/fence_wait.hpp"

namespace openzoom {

enum class FrameReadiness { Ready, Busy, DeviceLost, WaitFailed };

// A latency event can consume a one-frame admission signal. Check the slot's
// fence first so a busy allocator cannot consume permission for a future draw.
// pollLatency must only poll (zero timeout), never wait on the UI thread.
template<class Completed, class DeviceLost, class PollLatency>
FrameReadiness PollFrameReadiness(std::uint64_t target,
                                  Completed completed,
                                  DeviceLost deviceLost,
                                  PollLatency pollLatency)
{
    const auto value = completed();
    if (value == std::numeric_limits<std::uint64_t>::max() || deviceLost())
        return FrameReadiness::DeviceLost;
    if (value < target) return FrameReadiness::Busy;
    const auto latency = pollLatency();
    if (latency == FenceEventResult::Failed) return FrameReadiness::WaitFailed;
    if (deviceLost()) return FrameReadiness::DeviceLost;
    return latency == FenceEventResult::Signaled
        ? FrameReadiness::Ready : FrameReadiness::Busy;
}

} // namespace openzoom

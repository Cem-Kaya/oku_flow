#pragma once

#include <chrono>

namespace okuflow {

// Owned by one transport worker. Packet arrival is not decoder progress.
class PhoneFrameProgress {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    enum class Failure { None, TransportStalled, DecoderStalled, HevcStalled };

    explicit PhoneFrameProgress(TimePoint now)
        : lastPacket_(now), lastFrame_(now), generationStarted_(now) {}

    void BeginGeneration(bool hevc, TimePoint now) noexcept {
        hevc_ = hevc;
        generationDecoded_ = false;
        generationStarted_ = now;
        // Repeated configuration changes must not postpone the stream deadline.
    }
    void PacketReceived(TimePoint now) noexcept { lastPacket_ = now; }
    void FrameDecoded(TimePoint now) noexcept {
        generationDecoded_ = true;
        lastFrame_ = now;
    }
    Failure Check(TimePoint now) const noexcept {
        if (hevc_ && !generationDecoded_ &&
            now - generationStarted_ >= std::chrono::seconds(4))
            return Failure::HevcStalled;
        if (now - lastPacket_ >= std::chrono::seconds(12))
            return Failure::TransportStalled;
        if (now - lastFrame_ >= std::chrono::seconds(12))
            return hevc_ ? Failure::HevcStalled : Failure::DecoderStalled;
        return Failure::None;
    }

private:
    TimePoint lastPacket_, lastFrame_, generationStarted_;
    bool hevc_{false};
    bool generationDecoded_{false};
};

} // namespace okuflow

#pragma once

#ifdef _WIN32
#include "okuflow/common/video_playback.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace okuflow {

struct VideoAudioOutputSnapshot {
    std::uint64_t requestGeneration{};
    bool available{}, clockValid{}, playing{}, muted{true}, flushPending{};
    // Device-consumed media time, never the PTS of merely queued PCM. The
    // timestamp uses steady_clock's epoch; callers must reject stale samples.
    std::int64_t renderedPts100ns{}, sampledAtSteady100ns{};
    // Includes the packet held by the worker, even after cancellation until
    // that worker releases it. WASAPI's separate buffer is at most 250 ms.
    std::size_t queuedBytes{}, queuedPackets{}, inFlightBytes{};
    std::int64_t queuedDuration100ns{}, deviceBufferedDuration100ns{};
    std::uint64_t discardedPackets{};
    std::string error;
};

// A single, bounded WASAPI shared-mode output worker. No method invokes COM,
// waits for a driver, joins a thread, or changes microphone/recording routing.
// The caller must mute output during recording and recording-session review.
class VideoAudioOutput {
public:
    struct State;
    struct Options {
        // False is a deterministic admission-only seam: no thread, COM,
        // endpoint access, or sound. Snapshot.available remains false.
        bool nativePlayback{true};
    };
    static constexpr std::size_t kByteLimit = 2u * 1024 * 1024;
    static constexpr std::size_t kPacketLimit = 32;
    static constexpr std::int64_t kDurationLimit100ns = 5'000'000;

    VideoAudioOutput();
    explicit VideoAudioOutput(Options options);
    ~VideoAudioOutput(); // cancellation only, including a hung native call
    VideoAudioOutput(const VideoAudioOutput&) = delete;
    VideoAudioOutput& operator=(const VideoAudioOutput&) = delete;

    // A changed generation, pause, or mute invalidates queued and in-flight
    // publication immediately; native Stop/Reset is asynchronous. Repeating
    // an unchanged configuration does not reset the playback clock.
    void Configure(std::uint64_t requestGeneration, bool playing, bool muted);
    // Takes owned, interleaved signed PCM16. Rejected/over-budget packets are
    // discarded; false requires the caller to use its video-clock fallback.
    bool Push(VideoPlaybackAudio packet);
    void Reset(std::uint64_t requestGeneration);
    void Close(); // mute, pause and flush; the same sink can be configured again
    VideoAudioOutputSnapshot Snapshot() const;

private:
    std::shared_ptr<State> state_;
};

} // namespace okuflow
#endif

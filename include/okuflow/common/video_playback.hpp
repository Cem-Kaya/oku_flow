#pragma once

#ifdef _WIN32
#include "okuflow/common/video_archive.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace okuflow {

struct VideoPlaybackFrame {
    std::vector<std::uint8_t> bgra;
    std::uint32_t width{}, height{}, strideBytes{};
    std::uint32_t rotationDegrees{}, pixelAspectNumerator{1}, pixelAspectDenominator{1};
    std::int64_t pts100ns{}, duration100ns{};
    std::uint64_t sampleId{}, requestGeneration{}, archiveGeneration{};
    std::uint32_t videoTrackId{};
};
struct VideoPlaybackAudio {
    std::vector<std::uint8_t> pcm;
    std::uint32_t sampleRate{}, channels{}, bitsPerSample{16};
    std::int64_t pts100ns{};
    std::uint64_t requestGeneration{};
};
enum class VideoPlaybackState { Closed, Opening, Paused, Playing, Seeking, Ended, Unavailable, Waiting };
struct VideoPlaybackSnapshot {
    VideoPlaybackState state{VideoPlaybackState::Closed};
    std::uint64_t requestGeneration{};
    std::shared_ptr<const VideoPlaybackFrame> frame;
    std::shared_ptr<const VideoPlaybackFrame> neighborFrame;
    std::wstring path;
    VideoFileIdentity file;
    std::uint32_t videoTrackId{};
    std::int64_t duration100ns{}, seekableEnd100ns{-1};
    std::size_t decodedBytes{}, peakDecodedBytes{};
    bool hasAudio{}, audioAvailable{}, muted{true}, incomplete{}, activeRecording{};
    std::string error;
};

// Pure resource-admission seam. The 96 MiB pool includes frames retained by
// consumers: keeping old snapshots can make a new decode unavailable.
bool VideoPlaybackLayoutFits(std::uint32_t width, std::uint32_t height,
                             std::int64_t strideBytes);
// Nonnegative presentation interval representable by the nanosecond host
// clock. Checks the sum without signed overflow; zero duration is unknown.
bool VideoPlaybackTimestampFits(std::int64_t pts100ns,
                               std::int64_t duration100ns = 0);

class VideoPlayback {
public:
    struct State;
    static constexpr std::size_t kDecodedByteLimit = 96u * 1024 * 1024;
    static constexpr std::size_t kReadWindowByteLimit = 64u * 1024 * 1024;
    static constexpr std::size_t kAudioByteLimit = 2u * 1024 * 1024;
    static constexpr std::uint32_t kSeekSampleLimit = 120;
    VideoPlayback();
    ~VideoPlayback(); // cancellation only; never joins a native call
    VideoPlayback(const VideoPlayback&) = delete;
    VideoPlayback& operator=(const VideoPlayback&) = delete;
    std::uint64_t OpenFile(std::wstring path);
    std::uint64_t OpenArchive(std::shared_ptr<RecordingArchive> archive);
    std::uint64_t Seek100ns(std::int64_t target);
    std::uint64_t JumpToEnd();
    void SetPlaying(bool playing);
    void SetMuted(bool muted);
    // Consumed output clock in the same steady-clock epoch as
    // duration_cast<nanoseconds>(steady_clock::now().time_since_epoch())/100.
    // Fresh samples (<=500 ms) pace video; stale/muted samples use host time.
    // A negative pts explicitly invalidates the current generation's clock.
    void SetAudioClock100ns(std::int64_t pts, std::int64_t sampledAtSteady100ns,
                           std::uint64_t requestGeneration);
    void Close();
    VideoPlaybackSnapshot Snapshot() const;
    // At most 2 MiB / 32 packets / 500 ms queued. Only current-generation
    // packets are returned. Pause/seek/close/mute discard pending output.
    std::vector<VideoPlaybackAudio> TakeAudio();
private:
    std::shared_ptr<State> state_;
};

} // namespace okuflow
#endif

#pragma once

#ifdef _WIN32
#include "okuflow/common/video_archive.hpp"
#include "okuflow/common/video_gpu_decoder.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace okuflow {

struct VideoPlaybackFrame {
    // Immutable pixels can be reticketed after a same-frame seek without a
    // second image allocation. The final pixel lease owns the budget charge.
    std::shared_ptr<const std::vector<std::uint8_t>> bgra;
    std::shared_ptr<const VideoPlaybackGpuImage> gpu;
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
    std::size_t decodedBytes{}, peakDecodedBytes{}, decodedByteLimit{}, decodeWorkingBytes{};
    std::uint32_t readyFrames{}, lookaheadFrames{}, lastRequestReadCount{}, lastRequestCopiedFrames{};
    std::int64_t lastRequestLatencyUs{};
    bool memoryPressure{};
    bool lastRequestReusedFrame{};
    bool gpuResident{};
    std::uint32_t gpuDecodedBitsPerComponent{};
    std::size_t gpuTextureBytes{};
    std::string backendReason;
    bool hasAudio{}, audioAvailable{}, muted{true}, incomplete{}, activeRecording{};
    std::string error;
};

struct VideoPlaybackMemoryPlan {
    std::size_t stagingBytes{}, frameBytes{}, workingBytes{}, retainedBytes{}, byteLimit{};
    std::uint32_t lookaheadFrames{};
};
// Checked, resolution/stride-aware admission under the absolute decoded cap.
// A decode starts only with credit for staging + its replacement; retained
// leases are charged and apply interruptible backpressure when credit is tight.
std::optional<VideoPlaybackMemoryPlan> PlanVideoPlaybackMemory(
    std::uint32_t width, std::uint32_t height, std::int64_t strideBytes,
    std::uint32_t pixelAspectNumerator = 1, std::uint32_t pixelAspectDenominator = 1,
    bool activeArchive = false);
bool VideoPlaybackLayoutFits(std::uint32_t width, std::uint32_t height,
                             std::int64_t strideBytes);
// Nonnegative presentation interval representable by the nanosecond host
// clock. Checks the sum without signed overflow; zero duration is unknown.
bool VideoPlaybackTimestampFits(std::int64_t pts100ns,
                               std::int64_t duration100ns = 0);

class VideoPlayback {
public:
    struct State;
    static constexpr std::size_t kMinimumDecodedByteLimit = 96u * 1024 * 1024;
    static constexpr std::size_t kDecodedByteLimit = 256u * 1024 * 1024;
    static constexpr std::size_t kReadWindowByteLimit = 64u * 1024 * 1024;
    static constexpr std::size_t kAudioByteLimit = 2u * 1024 * 1024;
    // Long-GOP screen recordings can require hundreds of cheap preroll
    // reads. The two-second wall deadline and generation cancellation are
    // the latency bounds; this independent guard stops malformed streams.
    static constexpr std::uint32_t kSeekSampleLimit = 4096;
    VideoPlayback();
    ~VideoPlayback(); // cancellation only; never joins a native call
    VideoPlayback(const VideoPlayback&) = delete;
    VideoPlayback& operator=(const VideoPlayback&) = delete;
    std::uint64_t OpenFile(std::wstring path);
    std::uint64_t OpenArchive(std::shared_ptr<RecordingArchive> archive);
    std::uint64_t Seek100ns(std::int64_t target);
    std::uint64_t JumpToEnd();
    // Applies to subsequent opens. The UI supplies the presenter's exact LUID
    // only when its CUDA output is available; empty selects CPU compatibility.
    void SetGpuAdapter(std::optional<LUID> adapter);
    // Explicitly revokes the GPU request and re-seeks the same document at its
    // held cursor. It remains CPU-only for that document until the next Open.
    std::uint64_t UseCpuFallback(std::string reason);
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

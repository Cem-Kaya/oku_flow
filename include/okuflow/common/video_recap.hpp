#pragma once
#ifdef _WIN32
#include "okuflow/common/media_writer.hpp"
#include "okuflow/common/video_memory_stream.hpp"
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace okuflow {
struct VideoRecapPolicy {
    static constexpr std::int64_t kWindow100ns = 300LL * 10'000'000;
    static constexpr std::int64_t kSegment100ns = 2LL * 10'000'000;
    static constexpr std::size_t kCompressedBytes = 768u * 1024 * 1024;
    static constexpr std::size_t kSegmentBytes = 8u * 1024 * 1024;
    static constexpr std::size_t kQueueBytes = 96u * 1024 * 1024;
    static constexpr unsigned kQueueFrames = 2, kPinnedSegments = 2;
    static constexpr unsigned kSegments = 160; // ~150 normal two-second segments plus bounded margin.
    std::uint32_t fps{}, bitrate{};
    static std::optional<VideoRecapPolicy> ForSize(std::uint32_t width, std::uint32_t height);
};
struct VideoRecapAdmission {
    std::uint64_t sequence{};
    std::int64_t capture100ns{}, pts100ns{};
};
struct VideoRecapSegment {
    std::shared_ptr<const VideoMemorySource> source;
    std::uint64_t epoch{}, id{};
    std::int64_t begin100ns{}, end100ns{}, captureOrigin100ns{};
    std::vector<VideoRecapAdmission> admissions;
};
struct VideoRecapSnapshot {
    std::uint64_t epoch{}, droppedFrames{}, admittedFrames{}, completedFrames{}, completedSegments{};
    std::int64_t first100ns{-1}, end100ns{-1}, latestCapture100ns{-1}, captureOrigin100ns{-1};
    std::size_t compressedBytes{}, queuedBytes{}, pinnedSegments{};
    std::uint32_t queuedFrames{};
    std::int64_t maximumNativeCallUs{};
    bool available{}, pressure{}, warming{true};
    std::string error;
};
// Pure segment accounting. Segments are independently coded access points;
// whole-segment eviction never leaves inter-predicted orphan packets. A lease
// pins at most two immutable segments outside the ring, across epoch resets.
class VideoRecapRing {
public:
    struct Pins;
    explicit VideoRecapRing(std::size_t byteLimit = VideoRecapPolicy::kCompressedBytes,
                           std::int64_t window100ns = VideoRecapPolicy::kWindow100ns);
    void Reset(std::uint64_t epoch);
    void SetWindow(std::int64_t window100ns);
    bool Append(std::shared_ptr<const VideoRecapSegment> segment);
    std::shared_ptr<const VideoRecapSegment> Acquire(std::int64_t target100ns) const;
    VideoRecapSnapshot Snapshot() const;
private:
    mutable std::mutex mutex_;
    std::deque<std::shared_ptr<const VideoRecapSegment>> segments_;
    std::shared_ptr<Pins> pins_;
    std::uint64_t epoch_{};
    std::size_t bytes_{}, byteLimit_;
    std::int64_t window100ns_;
};
struct VideoRecapFrame {
    std::shared_ptr<const std::vector<std::uint8_t>> pixels;
    GpuVideoFrame gpu;
    std::uint32_t width{}, height{};
    RecordingFrameIdentity identity;
};
// One detached, session-owned native worker. Public methods never invoke an
// encoder or join a native call. No path, file archive or audio input exists.
class RamVideoRecap {
public:
    struct State;
    RamVideoRecap();
    ~RamVideoRecap();
    void Reset(std::uint64_t cameraEpoch, int rotationQuarterTurns);
    void SetDurationMinutes(int minutes);
    bool WantsFrame(std::uint32_t width, std::uint32_t height,
                    std::int64_t capture100ns) const;
    bool Submit(VideoRecapFrame frame);
    std::shared_ptr<const VideoRecapSegment> Acquire(std::int64_t target100ns) const;
    VideoRecapSnapshot Snapshot() const;
private:
    friend struct RamVideoRecapTestAccess;
    // Private fixture seam, installed before input. No production selector.
    bool SetWriterSetupForTest(std::function<void(VideoRecorder&)> setup);
    std::shared_ptr<State> state_;
};
} // namespace okuflow
#endif

#pragma once

#ifdef _WIN32

#include "okuflow/common/media_writer.hpp"
#include "okuflow/common/recording_contract.hpp"
#include "okuflow/common/transcript.hpp"
#include "okuflow/app/user_data_paths.hpp"
#include "okuflow/capture/audio_capture.hpp"

#include <QString>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <vector>

QT_BEGIN_NAMESPACE
class QPushButton;
class QTimer;
QT_END_NAMESPACE

namespace okuflow {

struct RecordingTimingSnapshot {
    float p50Ms{-1.0f};
    float p95Ms{-1.0f};
    float p99Ms{-1.0f};
    std::size_t sampleCount{0};

    bool IsValid() const noexcept { return sampleCount > 0; }
};

struct CapturedFrame {
    std::vector<uint8_t> pixels;
    GpuVideoFrame gpuScene;
    UINT width{};
    UINT height{};
    RecordingFrameIdentity identity{};

    bool HasCpuPixels() const {
        return !pixels.empty() && width > 0 && height > 0;
    }

    bool HasGpuScene() const {
        return gpuScene.IsValid() &&
               gpuScene.width == width &&
               gpuScene.height == height;
    }

    bool IsValid() const {
        return width > 0 && height > 0 &&
               (HasCpuPixels() || HasGpuScene());
    }
};

struct SavedRecordingSegment {
    RecordingSessionInfo session;
    int segmentIndex{1};
    QString originalPath;
    QString processedPath;
};

class RecordingManager {
public:
    using StatusCallback = std::function<void(const QString&, int)>;
    using SegmentSavedCallback =
        std::function<void(const SavedRecordingSegment&)>;
    using SessionEndedCallback =
        std::function<void(const RecordingSessionInfo&)>;

    RecordingManager(QPushButton* recordButton,
                     StatusCallback statusCallback,
                     SegmentSavedCallback segmentSavedCallback,
                     const UserDataPaths& userDataPaths,
                     SessionEndedCallback sessionEndedCallback = {});
    ~RecordingManager();

    // Bounded shutdown handshake for app close. Returns true when the worker
    // exited and was joined — destroying the manager is then safe, though the
    // caller must still consult sticky IsWorkerAbandoned() before global
    // MF/COM teardown because poisoned recorder objects are leaked. Returns
    // false when the worker is wedged inside a synchronous encoder/driver
    // call: both recorders are abandoned and the worker is detached, so the
    // caller MUST leak this object (unique_ptr::release) instead of
    // destroying it — the detached worker still references its queues and
    // atomics, and freeing them would hand it dangling memory if the
    // blocked driver call ever returns before process exit.
    bool ShutdownForProcessExit();

    void SetRequested(bool requested);
    void Stop(const QString& message = {});
    void SetCanvasMode(RecordingCanvasMode mode);
    RecordingCanvasSize ResolveCanvas(UINT sourceWidth,
                                      UINT sourceHeight) const;
    void SetAudioCaptureEnabled(bool enabled);
    void AddAudioFrame(AudioFrame frame);

    bool IsActive() const;
    RecordingState State() const { return state_.load(); }
    // Identity of the active session; empty while not recording. Transcript
    // sessions key their durable output to this id.
    std::optional<RecordingSessionInfo> CurrentSessionInfo() const;
    QString CodecName() const;
    RecordingTimingSnapshot EncoderSubmitTiming() const;
    // True after the stop watchdog declared the recording worker permanently
    // blocked inside a synchronous encoder/driver call. Recording is then
    // unavailable for the rest of the process lifetime; everything else in
    // the app keeps working.
    bool IsWorkerAbandoned() const { return workerAbandoned_.load(); }
    // Human-readable snapshot of the exact call the worker is executing,
    // readable from any thread even while the worker is blocked.
    QString DescribeWorkerStage() const;

    void NotifyCaptureFrame(bool overwrotePendingPreview);
    // Counts asynchronous D3D11 completion deferrals separately from drops.
    // A retry remains GPU-resident and should normally complete on the next
    // event-loop turn.
    void NotifyCaptureGpuRetry();
    // Counts frames that crossed system memory because the GPU completion
    // retry budget expired or the accelerated path was unavailable.
    void NotifyCaptureSafeCopyFallback();
    void NotifyReadbackSkipped();
    void NotifyRecordingPoolSkipped();
    void StorePendingOriginal(UINT64 requestId,
                              CapturedFrame frame,
                              const RecordingViewTransform& transform);
    bool HandleProcessedReadback(UINT64 requestId,
                                 const uint8_t* processedData,
                                 UINT processedWidth,
                                 UINT processedHeight);
    void ClearPendingReadbacks();
    void AddSceneFrame(const uint8_t* processedSceneData,
                       UINT processedSceneWidth,
                       UINT processedSceneHeight,
                       const RecordingViewTransform& transform,
                       CapturedFrame originalFrame);
    void AddGpuSceneFrame(GpuVideoFrame processedScene,
                          const RecordingViewTransform& transform,
                          CapturedFrame originalFrame);

private:
    struct PendingOriginal {
        CapturedFrame frame;
        RecordingViewTransform transform;
    };

    struct QueuedFrame {
        std::vector<uint8_t> processedScene;
        GpuVideoFrame processedGpuScene;
        UINT processedSceneWidth{};
        UINT processedSceneHeight{};
        RecordingViewTransform transform;
        CapturedFrame original;
    };

    struct SegmentParameters {
        UINT processedWidth{};
        UINT processedHeight{};
        UINT originalWidth{};
        UINT originalHeight{};
        UINT frameRateNumerator{30};
        UINT frameRateDenominator{1};
    };

    // Which recorder call the worker thread is inside. Combined with each
    // VideoRecorder's fine-grained WriterStage this names the blocked call.
    enum class WorkerOp : std::uint8_t {
        kIdle = 0,
        kStartSegment,
        kProcessedVideo,
        kOriginalVideo,
        kAudio,
        kFinalize,
    };
    static const char* WorkerOpName(WorkerOp op);

    void WorkerLoop();
    void SetWorkerOp(WorkerOp op, std::uint64_t frameSequence);
    void OnStopWatchdogFired(std::uint32_t generation);
    void OnWorkerHeartbeat();
    void AbandonWedgedWorker(const QString& trigger);
    bool StartSegment(const QueuedFrame& firstFrame);
    bool WriteFrame(QueuedFrame&& frame);
    void RecordEncoderSubmitTiming(float milliseconds);
    bool WriteAudioFrame(AudioFrame&& frame);
    bool FinalizeSegment(bool terminal);
    void FinishSession(bool success, const QString& detail = {});
    void SetState(RecordingState state);
    void PostStatus(const QString& message, int durationMs = 7000) const;
    void PostButtonState(RecordingState state) const;
    void PostSegmentSaved(const QString& originalPath,
                          const QString& processedPath) const;
    QString BuildDropSummary() const;
    QString SegmentPath(bool processed) const;
    RecordingDropCounts DropCounts() const;
    QString EnsureOutputDirectory() const;
    void ShowStatus(const QString& message, int durationMs = 7000) const;
    void UpdateButton();

    QPushButton* recordButton_{};
    StatusCallback statusCallback_;
    SegmentSavedCallback segmentSavedCallback_;
    SessionEndedCallback sessionEndedCallback_;
    const UserDataPaths* userDataPaths_{};
    VideoRecorder processedRecorder_;
    VideoRecorder originalRecorder_;
    std::unordered_map<UINT64, PendingOriginal> pendingOriginalReadbacks_;
    mutable std::mutex pendingMutex_;
    std::atomic<RecordingState> state_{RecordingState::Idle};
    std::atomic<RecordingCanvasMode> canvasMode_{
        RecordingCanvasMode::Source};
    QString sessionTimestamp_;
    RecordingSessionInfo sessionInfo_;  // guarded by queueMutex_
    QString sessionDirectory_;
    QString codecName_;
    QString stopMessage_;
    std::thread worker_;
    mutable std::mutex queueMutex_;
    std::condition_variable queueCv_;
    std::deque<QueuedFrame> frameQueue_;
    std::deque<AudioFrame> audioQueue_;
    bool stopRequested_{false};
    bool shutdownRequested_{false};
    bool workerWriting_{false};
    bool workerDone_{false};
    static constexpr std::size_t kMaxQueuedFrames = 12;
    static constexpr std::size_t kMaxQueuedAudioFrames = 512;
    // Stop must complete or fail visibly within a bounded time: normal stops
    // finish in well under 5 s; the watchdog fires at 8 s so it can never
    // race a legitimately slow-but-progressing finalize.
    static constexpr int kStopWatchdogMs = 8000;
    static constexpr int kShutdownJoinMs = 3000;
    // A live-session heartbeat covers wedges the stop watchdog cannot see
    // (for example a mode-change finalize blocking mid-recording): any
    // single recorder call still running after 10 s is a wedge — normal
    // submits are milliseconds.
    static constexpr int kHeartbeatIntervalMs = 2000;
    static constexpr int kWorkerWedgeMs = 10000;
    QTimer* heartbeatTimer_{};
    std::atomic<WorkerOp> workerOp_{WorkerOp::kIdle};
    std::atomic<std::int64_t> workerOpStartedMs_{0};
    std::atomic<std::uint64_t> workerOpFrameSequence_{0};
    std::atomic<std::uint32_t> stopGeneration_{0};
    std::atomic<bool> workerAbandoned_{false};
    // Set on the ShutdownForProcessExit wedge path; makes the destructor's
    // leak-contract diagnostic accurate even though detach() clears
    // joinable().
    bool workerDetached_{false};
    std::atomic<std::uint64_t> captureFramesSeen_{0};
    std::atomic<std::uint64_t> framesWritten_{0};
    std::atomic<std::uint64_t> captureSlotOverwrite_{0};
    std::atomic<std::uint64_t> captureGpuRetry_{0};
    std::atomic<std::uint64_t> captureSafeCopyFallback_{0};
    std::atomic<std::uint64_t> readbackRingBusy_{0};
    std::atomic<std::uint64_t> recordingPoolBusy_{0};
    std::atomic<std::uint64_t> pairingMiss_{0};
    std::atomic<std::uint64_t> queueOverflow_{0};
    std::atomic<std::uint64_t> encoderReject_{0};
    std::atomic<std::uint64_t> unknownTimestamp_{0};
    std::atomic<std::uint64_t> gpuFramesWritten_{0};
    std::atomic<std::uint64_t> gpuInputFallbacks_{0};
    std::atomic<std::uint64_t> processedFrameFallbacks_{0};
    std::atomic<std::uint64_t> originalFrameFallbacks_{0};
    static constexpr std::size_t kTimingSampleCapacity = 240;
    mutable std::mutex timingMutex_;
    std::array<float, kTimingSampleCapacity> encoderSubmitSamplesMs_{};
    std::size_t encoderSubmitSampleIndex_{0};
    std::size_t encoderSubmitSampleCount_{0};
    UINT processedWidth_{};
    bool segmentFinalized_{true};
    UINT processedHeight_{};
    UINT originalWidth_{};
    UINT originalHeight_{};
    UINT frameRateNumerator_{30};
    UINT frameRateDenominator_{1};
    std::int64_t segmentClockOrigin100ns_{-1};
    std::atomic<bool> audioCaptureEnabled_{false};
    int segmentIndex_{1};
    bool sessionHadOutput_{false};
    bool sessionFinalizationFailed_{false};
};

} // namespace okuflow

#endif // _WIN32

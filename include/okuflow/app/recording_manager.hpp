#pragma once

#ifdef _WIN32

#include "okuflow/common/media_writer.hpp"
#include "okuflow/common/annotation_model.hpp"
#include "okuflow/common/recording_contract.hpp"
#include "okuflow/app/recording_session_info.hpp"
#include "okuflow/app/user_data_paths.hpp"
#include "okuflow/capture/audio_capture.hpp"

#include <QString>
#include <QStringList>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

QT_BEGIN_NAMESPACE
class QPushButton;
class QTimer;
QT_END_NAMESPACE

namespace okuflow {

class RecordingArchive;

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
        return width > 0 && height > 0 &&
               width <= std::numeric_limits<std::size_t>::max() / height / 4u &&
               pixels.size() == static_cast<std::size_t>(width) * height * 4u;
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
    std::shared_ptr<RecordingArchive> archive;
    // Compatibility only: new sessions never create a processed companion.
    QString processedPath;
    std::int64_t firstCaptureClock100ns{-1};
    std::int64_t lastCaptureClock100ns{-1};
    std::uint64_t framesWritten{};
    RecordingSessionCompletion completion{RecordingSessionCompletion::Completed};
    QStringList failedAttemptPaths;
};

struct RecordingFrameAdmission {
    std::uint64_t sequenceNumber{};
    std::int64_t captureClock100ns{-1};
    std::int64_t pts100ns{-1};
};

// Immutable, bounded evidence of source frames accepted by this exact part's
// writer input. Acceptance can precede WriteSample; Archive() separately
// reports writer-committed and verified/seekable boundaries.
struct ActiveRecordingTimeline {
    RecordingSessionInfo session;
    int segmentIndex{1};
    std::shared_ptr<RecordingArchive> archive;
    std::int64_t firstCaptureClock100ns{-1};
    std::int64_t lastCaptureClock100ns{-1};
    std::uint64_t lastAdmittedSequence{};
    std::int64_t lastAdmittedPts100ns{-1};
    std::vector<RecordingFrameAdmission> recentAdmissions;

    std::optional<RecordingFrameAdmission> FindAdmission(
        std::uint64_t sequence, std::int64_t captureClock100ns) const {
        for (auto it = recentAdmissions.rbegin(); it != recentAdmissions.rend(); ++it)
            if (it->sequenceNumber == sequence && it->captureClock100ns == captureClock100ns)
                return *it;
        return std::nullopt;
    }
};

class RecordingManager {
public:
    using StatusCallback = std::function<void(const QString&, int)>;
    using SegmentSavedCallback =
        std::function<void(const SavedRecordingSegment&)>;
    using SessionEndedCallback =
        std::function<void(const RecordingSessionInfo&)>;
    // Queued once only after a successful first Original frame admission.
    // Stopped/failed/replaced sessions cannot deliver a late start.
    using SessionStartedCallback =
        std::function<void(const RecordingSessionInfo&)>;

    RecordingManager(QPushButton* recordButton,
                     StatusCallback statusCallback,
                     SegmentSavedCallback segmentSavedCallback,
                     const UserDataPaths& userDataPaths,
                     SessionEndedCallback sessionEndedCallback = {},
                     SessionStartedCallback sessionStartedCallback = {});
    ~RecordingManager();

    // Bounded shutdown handshake for app close. Returns true when the worker
    // exited and was joined — destroying the manager is then safe, though the
    // caller must still consult sticky IsWorkerAbandoned() before global
    // MF/COM teardown because poisoned recorder objects are leaked. Returns
    // false when the worker is wedged inside a synchronous encoder/driver
    // call: the recorder is abandoned and the worker is detached, so the
    // caller MUST leak this object (unique_ptr::release) instead of
    // destroying it — the detached worker still references its queues and
    // atomics, and freeing them would hand it dangling memory if the
    // blocked driver call ever returns before process exit.
    bool ShutdownForProcessExit();

    void SetRequested(bool requested, std::optional<RecordingSessionInfo> reserved = std::nullopt);
    bool StopSession(const QString& expectedSessionId);
    void SetSessionAudioPending(const QString& expectedSessionId);
    void SetSessionAudioResult(const QString& expectedSessionId, bool actualAudio);
    void Stop(const QString& message = {});
    void SetCanvasMode(RecordingCanvasMode mode);
    RecordingCanvasSize ResolveCanvas(UINT sourceWidth,
                                      UINT sourceHeight) const;
    void SetAudioCaptureEnabled(bool enabled);
    void AddAudioFrame(AudioFrame frame);

    bool IsActive() const;
    RecordingState State() const { return state_.load(); }
    // Identity of the active session; empty while not recording. Saved-media
    // callbacks retain this id independently of later sessions.
    std::optional<RecordingSessionInfo> CurrentSessionInfo() const;
    // Snapshots never call into an encoder on the caller's thread. The last
    // selected archive/receipt survive finalization until the next session.
    std::shared_ptr<RecordingArchive> CurrentArchive() const;
    std::optional<SavedRecordingSegment> LastSavedSegment() const;
    std::optional<ActiveRecordingTimeline> CurrentTimeline() const;
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
    // Notifications can coalesce different arrival and overwrite counts.
    void NotifyCaptureFrames(std::uint64_t received,
                             std::uint64_t overwritten);
    // Counts asynchronous D3D11 completion deferrals separately from drops.
    // A retry remains GPU-resident and should normally complete on the next
    // event-loop turn.
    void NotifyCaptureGpuRetry();
    // Counts frames that crossed system memory because the GPU completion
    // retry budget expired or the accelerated path was unavailable.
    void NotifyCaptureSafeCopyFallback();
    // The caller reports once when a fresh camera arrival is dropped because
    // upload slots are busy; completion retries and viewport presents do not
    // represent another dropped recording frame.
    void NotifyCaptureGpuUploadBusy();
    RecordingDropCounts DropCounts() const;
    void NotifyReadbackSkipped();
    void NotifyRecordingPoolSkipped();
    // Owns the clean full-source frame/lease independently of presentation.
    // Color conversion and source orientation are already normalized by the
    // producer; view effects, crop and annotations never enter this queue.
    void AddOriginalFrame(CapturedFrame originalFrame);
    // Transitional source compatibility. Processed payload/transform/ink are
    // ignored and never copied, resampled or submitted to an encoder.
    void AddSceneFrame(const uint8_t* processedSceneData,
                       UINT processedSceneWidth,
                       UINT processedSceneHeight,
                       const RecordingViewTransform& transform,
                       CapturedFrame originalFrame,
                       QVector<AnnotationStroke> strokes = {});
    void AddGpuSceneFrame(GpuVideoFrame processedScene,
                          const RecordingViewTransform& transform,
                          CapturedFrame originalFrame);

private:
    friend struct RecordingManagerTestAccess;
    struct QueuedFrame {
        CapturedFrame original;
    };

    struct SegmentParameters {
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
        kOriginalVideo,
        kAudio,
        kRetireGpuReaders,
        kFinalize,
    };
    static const char* WorkerOpName(WorkerOp op);

    void WorkerLoop();
    void RetireGpuInputReads();
    void SetWorkerOp(WorkerOp op, std::uint64_t frameSequence);
    void OnStopWatchdogFired(std::uint32_t generation);
    void OnWorkerHeartbeat();
    void AbandonWedgedWorker(const QString& trigger);
    bool StartSegment(const QueuedFrame& firstFrame);
    bool WriteFrame(QueuedFrame&& frame);
    bool CompleteFrameSubmission(bool accepted,
                                 const RecordingFrameIdentity& identity,
                                 std::int64_t admittedPts100ns,
                                 const QString& error = {});
    void PublishFrameAdmission(const RecordingFrameIdentity& identity,
                               std::int64_t admittedPts100ns);
    void RecordEncoderSubmitTiming(float milliseconds);
    bool WriteAudioFrame(AudioFrame&& frame);
    void ReportEncoderStatisticsUnavailable();
    bool FinalizeSegment(bool terminal);
    void FinishSession(bool success, const QString& detail = {});
    void SetState(RecordingState state);
    void PostStatus(const QString& message, int durationMs = 7000) const;
    void PostButtonState(RecordingState state) const;
    void PostSegmentSaved(const QString& originalPath,
                          std::uint64_t framesWritten);
    void PostSessionStarted();
    QString BuildDropSummary() const;
    QString SegmentPath() const;
    QString EnsureOutputDirectory() const;
    void ShowStatus(const QString& message, int durationMs = 7000) const;
    void UpdateButton();

    QPushButton* recordButton_{};
    StatusCallback statusCallback_;
    SegmentSavedCallback segmentSavedCallback_;
    SessionEndedCallback sessionEndedCallback_;
    SessionStartedCallback sessionStartedCallback_;
    const UserDataPaths* userDataPaths_{};
    VideoRecorder originalRecorder_;
    std::atomic<RecordingState> state_{RecordingState::Idle};
    // An immutable observer installed only by the desktop-free fixture. It
    // exposes the actual publication boundary without involving native codecs.
    std::function<void(RecordingState)> statePublicationObserver_;
    QString sessionTimestamp_;
    RecordingSessionInfo sessionInfo_;  // guarded by queueMutex_
    // A queued acknowledgement owns no manager pointer. Revocation on stop,
    // failure, replacement and shutdown fences exact session identity even
    // after this manager has been destroyed.
    struct SessionStartTicket {
        explicit SessionStartTicket(const QString& id) : sessionId(id) {}
        const QString sessionId;
        std::atomic<bool> active{true};
    };
    std::shared_ptr<SessionStartTicket> sessionStartTicket_; // queueMutex_
    bool sessionStartedPosted_{}; // queueMutex_: once per session, across parts
    std::shared_ptr<RecordingArchive> currentArchive_; // queueMutex_
    std::optional<SavedRecordingSegment> lastSavedSegment_; // queueMutex_
    std::optional<ActiveRecordingTimeline> activeTimeline_; // queueMutex_
    static constexpr std::size_t kMaxRecentAdmissions = 128;
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
    // Only wake an otherwise idle worker while conversion readers still hold
    // source leases. No polling timer runs for idle/CPU-only recording.
    static constexpr int kGpuReaderPollIntervalMs = 10;
    // Stop must complete or fail visibly within a bounded time: normal stops
    // finish in well under 5 s. This deadline is a response policy, not proof
    // of a driver hang: blocking Finalize exposes no worker-side progress, and
    // querying the same COM object from the UI would race the active call.
    static constexpr int kStopWatchdogMs = 8000;
    static constexpr int kShutdownJoinMs = 3000;
    // A live-session heartbeat covers wedges the stop watchdog cannot see
    // (for example a mode-change finalize blocking mid-recording): any
    // single recorder call still running after 10 s triggers abandonment.
    // Even with bounded admission a slow Finalize can reach this deadline.
    static constexpr int kHeartbeatIntervalMs = 2000;
    static constexpr int kWorkerWedgeMs = 10000;
    QTimer* heartbeatTimer_{};
    std::atomic<WorkerOp> workerOp_{WorkerOp::kIdle};
    std::atomic<std::int64_t> workerOpStartedMs_{0};
    std::atomic<std::uint64_t> workerOpFrameSequence_{0};
    std::atomic<std::uint32_t> stopGeneration_{0};
    std::atomic<bool> workerAbandoned_{false};
    // Queued successful callbacks cannot outlive an abandonment decision.
    std::shared_ptr<std::atomic<bool>> completionDeliveryEnabled_{
        std::make_shared<std::atomic<bool>>(true)};
    // Set on the ShutdownForProcessExit wedge path; makes the destructor's
    // leak-contract diagnostic accurate even though detach() clears
    // joinable().
    bool workerDetached_{false};
    std::atomic<std::uint64_t> captureFramesSeen_{0};
    std::atomic<std::uint64_t> framesWritten_{0};
    std::atomic<std::uint64_t> captureSlotOverwrite_{0};
    std::atomic<std::uint64_t> captureGpuRetry_{0};
    std::atomic<std::uint64_t> captureSafeCopyFallback_{0};
    std::atomic<std::uint64_t> captureGpuUploadBusy_{0};
    std::atomic<std::uint64_t> readbackRingBusy_{0};
    std::atomic<std::uint64_t> recordingPoolBusy_{0};
    std::atomic<std::uint64_t> queueOverflow_{0};
    std::atomic<std::uint64_t> audioQueueOverflow_{0};
    std::atomic<std::uint64_t> audioEncoderBacklog_{0};
    std::atomic<std::uint64_t> encoderReject_{0};
    std::atomic<std::uint64_t> encoderBacklog_{0};
    std::atomic<std::uint64_t> encoderStatisticsUnavailable_{0};
    std::atomic<bool> encoderStatisticsWarningActive_{false};
    std::atomic<bool> encoderBacklogWarningReported_{false};
    std::atomic<std::uint64_t> unknownTimestamp_{0};
    std::atomic<std::uint64_t> gpuFramesWritten_{0};
    std::atomic<std::uint64_t> gpuInputFallbacks_{0};
    std::atomic<std::uint64_t> originalFrameFallbacks_{0};
    static constexpr std::size_t kTimingSampleCapacity = 240;
    mutable std::mutex timingMutex_;
    std::array<float, kTimingSampleCapacity> encoderSubmitSamplesMs_{};
    std::size_t encoderSubmitSampleIndex_{0};
    std::size_t encoderSubmitSampleCount_{0};
    QString segmentOriginalPath_;
    QStringList segmentFailedAttemptPaths_;
    // Deterministic codec-failure fixture; normal instances use VideoRecorder.
    std::function<bool(const QString&, VideoRecorder::Codec)> encoderStartOverride_;
    bool segmentFinalized_{true};
    UINT originalWidth_{};
    UINT originalHeight_{};
    UINT frameRateNumerator_{30};
    UINT frameRateDenominator_{1};
    std::int64_t segmentClockOrigin100ns_{-1};
    std::int64_t segmentLastCaptureClock100ns_{-1};
    std::atomic<bool> audioCaptureEnabled_{false};
    bool audioStartupPending_{}; // queueMutex_: no segment before actual audio decision.
    int segmentIndex_{1};
    bool sessionHadOutput_{false};
    bool sessionFinalizationFailed_{false};
};

} // namespace okuflow

#endif // _WIN32

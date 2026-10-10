#ifdef _WIN32

#include "okuflow/app/recording_manager.hpp"
#include "okuflow/common/recording_preservation.hpp"
#include "okuflow/ui/live_status_text.hpp"

#include <QDate>
#include <QDateTime>
#include <QUuid>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStorageInfo>
#include <QStringList>
#include <QTimer>

#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <utility>

namespace okuflow {

namespace {

const char* StateName(RecordingState state)
{
    switch (state) {
    case RecordingState::Idle:
        return "Idle";
    case RecordingState::Starting:
        return "Starting";
    case RecordingState::Recording:
        return "Recording";
    case RecordingState::Stopping:
        return "Stopping";
    case RecordingState::Finalizing:
        return "Finalizing";
    case RecordingState::Completed:
        return "Completed";
    case RecordingState::Failed:
        return "Failed";
    }
    return "Unknown";
}

bool RecordingConsoleDiagnosticsEnabled()
{
    // The packaged GUI launch remains quiet. Launching from the development
    // terminal attaches a Windows console and enables concise route evidence.
    return GetConsoleWindow() != nullptr;
}

QString RecordingInputRoute(const VideoRecorder& recorder)
{
    if (recorder.UsesGpuInput()) {
        return QStringLiteral("direct GPU");
    }
    if (recorder.UsesWorkerGpuReadback()) {
        return QStringLiteral("GPU source with worker readback");
    }
    return QStringLiteral("CPU/system memory");
}

const char* FinalizeDispositionName(
    VideoRecorder::FinalizeDisposition disposition)
{
    switch (disposition) {
    case VideoRecorder::FinalizeDisposition::NothingToFinalize:
        return "nothing";
    case VideoRecorder::FinalizeDisposition::Completed:
        return "completed";
    case VideoRecorder::FinalizeDisposition::CompletedTruncated:
        return "truncated";
    }
    return "unknown";
}

QString FinalizeFailureText(
    const QString& label,
    const VideoRecorder::FinalizeResult& result)
{
    if (result.disposition !=
        VideoRecorder::FinalizeDisposition::CompletedTruncated) {
        return {};
    }
    return QStringLiteral(
               "%1 could not finish its final fragment (0x%2). The file "
               "was retained; playback has not been verified.")
        .arg(label)
        .arg(static_cast<qulonglong>(
                 static_cast<unsigned long>(result.hresult)),
             8,
             16,
             QLatin1Char('0'));
}

void RemoveEmptySessionRecording(const QString& path,
                                 std::uint64_t acceptedSamples = 0)
{
    const QFileInfo file(path);
    if (CanRemoveEmptyRecording(true, file.isFile(), file.isSymLink(),
                                file.size(), acceptedSamples)) {
        QFile::remove(path);
    }
}

} // namespace

void RecordingManager::RecordEncoderSubmitTiming(float milliseconds)
{
    if (!std::isfinite(milliseconds) || milliseconds < 0.0f) {
        return;
    }
    std::lock_guard lock(timingMutex_);
    encoderSubmitSamplesMs_[encoderSubmitSampleIndex_] = milliseconds;
    encoderSubmitSampleIndex_ =
        (encoderSubmitSampleIndex_ + 1) % encoderSubmitSamplesMs_.size();
    encoderSubmitSampleCount_ =
        std::min(encoderSubmitSampleCount_ + 1,
                 encoderSubmitSamplesMs_.size());
}

RecordingTimingSnapshot RecordingManager::EncoderSubmitTiming() const
{
    std::array<float, kTimingSampleCapacity> samples{};
    std::size_t count = 0;
    {
        std::lock_guard lock(timingMutex_);
        count = encoderSubmitSampleCount_;
        std::copy_n(encoderSubmitSamplesMs_.begin(), count, samples.begin());
    }
    RecordingTimingSnapshot result;
    result.sampleCount = count;
    if (count == 0) {
        return result;
    }
    std::sort(samples.begin(), samples.begin() +
                                  static_cast<std::ptrdiff_t>(count));
    const auto percentile = [&](double fraction) {
        const std::size_t index = static_cast<std::size_t>(
            std::ceil(fraction * static_cast<double>(count)) - 1.0);
        return samples[std::min(index, count - 1)];
    };
    result.p50Ms = percentile(0.50);
    result.p95Ms = percentile(0.95);
    result.p99Ms = percentile(0.99);
    return result;
}

RecordingManager::RecordingManager(QPushButton* recordButton,
                                   StatusCallback statusCallback,
                                   SegmentSavedCallback segmentSavedCallback,
                                   const UserDataPaths& userDataPaths,
                                   SessionEndedCallback sessionEndedCallback)
    : recordButton_(recordButton),
      statusCallback_(std::move(statusCallback)),
      segmentSavedCallback_(std::move(segmentSavedCallback)),
      sessionEndedCallback_(std::move(sessionEndedCallback)),
      userDataPaths_(&userDataPaths),
      worker_(&RecordingManager::WorkerLoop, this)
{
    if (recordButton_) {
        // Live-session heartbeat (see kWorkerWedgeMs). Owned by the button so
        // it lives on the UI thread and dies with the UI.
        heartbeatTimer_ = new QTimer(recordButton_);
        heartbeatTimer_->setInterval(kHeartbeatIntervalMs);
        QObject::connect(heartbeatTimer_, &QTimer::timeout, recordButton_,
                         [this]() { OnWorkerHeartbeat(); });
    }
    UpdateButton();
}

bool RecordingManager::ShutdownForProcessExit()
{
    {
        std::lock_guard lock(queueMutex_);
        shutdownRequested_ = true;
        stopRequested_ = true;
        frameQueue_.clear();
        audioQueue_.clear();
    }
    queueCv_.notify_all();
    if (!worker_.joinable()) {
        return !workerDetached_;
    }
    // An unbounded join here reproduced the shipped hang at app close when
    // the worker was wedged inside a synchronous encoder/driver call. Wait a
    // bounded time for the worker to acknowledge shutdown, then poison and
    // detach it. Detaching alone is not enough: the worker still references
    // this object, so on the false return the caller must leak the manager
    // (unique_ptr::release) — an unwedged worker then finds valid memory,
    // abandoned recorders, and cleared queues, and exits without touching
    // Qt or COM.
    bool done = false;
    {
        std::unique_lock lock(queueMutex_);
        done = queueCv_.wait_for(
            lock,
            std::chrono::milliseconds(kShutdownJoinMs),
            [this]() { return workerDone_; });
    }
    if (done) {
        worker_.join();
        return true;
    }
    qCritical() << "Recording worker did not shut down within"
                << kShutdownJoinMs << "ms; blocked at"
                << DescribeWorkerStage()
                << "- abandoning the recorders and detaching the worker; "
                   "the manager must now be leaked for process exit.";
    // Order matters: the abandoned flag silences every worker-side posting
    // and segment path before the recorders' COM teardown is redirected to
    // an intentional leak.
    workerAbandoned_.store(true);
    processedRecorder_.MarkAbandoned();
    originalRecorder_.MarkAbandoned();
    workerDetached_ = true;
    worker_.detach();
    return false;
}

RecordingManager::~RecordingManager()
{
    ShutdownForProcessExit();
    if (workerDetached_) {
        // Reachable only when a caller destroys the manager directly instead
        // of leaking it after a false ShutdownForProcessExit. The recorders
        // are already abandoned (their COM state leaks safely), but the
        // detached worker still references this object's queues; freeing
        // them here is the residual hazard the leak contract exists to
        // avoid.
        qCritical() << "RecordingManager destroyed while its worker is "
                       "wedged; leak the manager via ShutdownForProcessExit "
                       "instead.";
    }
}

bool RecordingManager::IsActive() const
{
    const RecordingState state = state_.load();
    return state == RecordingState::Starting ||
           state == RecordingState::Recording;
}

std::optional<RecordingSessionInfo> RecordingManager::CurrentSessionInfo() const
{
    if (!IsActive()) {
        return std::nullopt;
    }
    std::lock_guard lock(queueMutex_);
    if (sessionInfo_.id.isEmpty()) {
        return std::nullopt;
    }
    return sessionInfo_;
}

QString RecordingManager::CodecName() const
{
    std::lock_guard lock(queueMutex_);
    return codecName_;
}

void RecordingManager::SetState(RecordingState state)
{
    const RecordingState previous = state_.load();
    if (!IsValidRecordingStateTransition(previous, state)) {
        qWarning() << "Rejected invalid recording state transition:"
                   << StateName(previous) << "->" << StateName(state);
        return;
    }
    state_.store(state);
    qInfo() << "Recording state:" << StateName(state);
}

void RecordingManager::SetCanvasMode(RecordingCanvasMode mode)
{
    canvasMode_.store(mode);
}

RecordingCanvasSize RecordingManager::ResolveCanvas(
    UINT sourceWidth,
    UINT sourceHeight) const
{
    return ResolveRecordingCanvas(
        canvasMode_.load(), sourceWidth, sourceHeight);
}

void RecordingManager::SetAudioCaptureEnabled(bool enabled)
{
    std::lock_guard lock(queueMutex_);
    audioCaptureEnabled_ = enabled;
    if (!enabled) {
        audioQueue_.clear();
    }
}

void RecordingManager::AddAudioFrame(AudioFrame frame)
{
    if (!IsActive() || frame.pcm.empty() ||
        frame.captureClock100ns < 0 || frame.duration100ns <= 0) {
        return;
    }
    std::unique_lock lock(queueMutex_);
    if (!audioCaptureEnabled_ || stopRequested_ || shutdownRequested_) {
        return;
    }
    if (audioQueue_.size() >= kMaxQueuedAudioFrames) {
        audioQueue_.pop_front();
        queueOverflow_.fetch_add(1);
    }
    audioQueue_.push_back(std::move(frame));
    lock.unlock();
    queueCv_.notify_all();
}

void RecordingManager::SetRequested(bool requested)
{
    if (!requested) {
        Stop();
        return;
    }

    if (workerAbandoned_.load()) {
        // The worker thread is permanently blocked inside an encoder or
        // driver call and its recorders must not be reused. Everything else
        // in the app keeps working.
        ShowStatus(QStringLiteral(
            "Recording is unavailable because the video encoder stopped "
            "responding earlier. Restart OkuFlow to record again."));
        UpdateButton();
        return;
    }

    const RecordingState current = state_.load();
    if (current == RecordingState::Starting ||
        current == RecordingState::Recording ||
        current == RecordingState::Stopping ||
        current == RecordingState::Finalizing) {
        UpdateButton();
        return;
    }

    const QString outputDirectory = EnsureOutputDirectory();
    if (outputDirectory.isEmpty()) {
        ShowStatus(QStringLiteral(
            "Recording could not start because the OkuFlow recordings "
            "folder is unavailable."));
        UpdateButton();
        return;
    }
    const QStorageInfo storage(outputDirectory);
    constexpr qint64 kMinimumFreeBytes = 1024LL * 1024LL * 1024LL;
    if (!storage.isValid() || !storage.isReady()) {
        ShowStatus(QStringLiteral(
            "Recording could not start because OkuFlow could not check "
            "the destination drive."));
        UpdateButton();
        return;
    }
    if (storage.bytesAvailable() < kMinimumFreeBytes) {
        ShowStatus(QStringLiteral(
            "Recording could not start: less than 1 GB is free in the "
            "OkuFlow folder."));
        UpdateButton();
        return;
    }

    {
        std::lock_guard lock(queueMutex_);
        sessionTimestamp_ =
            QDateTime::currentDateTime().toString(
                QStringLiteral("yyyyMMdd_HHmmss_zzz"));
        sessionInfo_ = RecordingSessionInfo{
            QUuid::createUuid().toString(QUuid::WithoutBraces),
            sessionTimestamp_};
        sessionDirectory_ = outputDirectory;
        codecName_.clear();
        stopMessage_.clear();
        frameQueue_.clear();
        audioQueue_.clear();
        stopRequested_ = false;
        segmentIndex_ = 1;
        sessionHadOutput_ = false;
        sessionFinalizationFailed_ = false;
        processedWidth_ = 0;
        segmentFinalized_ = true;
        processedHeight_ = 0;
        originalWidth_ = 0;
        originalHeight_ = 0;
        frameRateNumerator_ = 30;
        frameRateDenominator_ = 1;
        segmentClockOrigin100ns_ = -1;
    }
    ClearPendingReadbacks();
    captureFramesSeen_.store(0);
    framesWritten_.store(0);
    captureSlotOverwrite_.store(0);
    captureGpuRetry_.store(0);
    captureSafeCopyFallback_.store(0);
    readbackRingBusy_.store(0);
    recordingPoolBusy_.store(0);
    pairingMiss_.store(0);
    queueOverflow_.store(0);
    encoderReject_.store(0);
    unknownTimestamp_.store(0);
    gpuFramesWritten_.store(0);
    gpuInputFallbacks_.store(0);
    processedFrameFallbacks_.store(0);
    originalFrameFallbacks_.store(0);
    {
        std::lock_guard lock(timingMutex_);
        encoderSubmitSamplesMs_.fill(0.0f);
        encoderSubmitSampleIndex_ = 0;
        encoderSubmitSampleCount_ = 0;
    }
    SetState(RecordingState::Starting);
    if (heartbeatTimer_) {
        heartbeatTimer_->start();
    }
    UpdateButton();
    ShowStatus(QStringLiteral("Recording is starting..."), 2500);
}

void RecordingManager::Stop(const QString& message)
{
    const RecordingState current = state_.load();
    if (current != RecordingState::Starting &&
        current != RecordingState::Recording) {
        return;
    }
    {
        std::lock_guard lock(queueMutex_);
        stopRequested_ = true;
        stopMessage_ = message;
        // Stop means finish the sample currently owned by the worker, then
        // finalize. Retaining queued camera/audio work here made a button
        // press wait seconds and could start another mode-change segment.
        frameQueue_.clear();
        audioQueue_.clear();
    }
    SetState(RecordingState::Stopping);
    UpdateButton();
    // "Finishing" is announced only when finalization actually begins
    // (FinalizeSegment). Until then the truthful state is "stopping".
    ShowStatus(QStringLiteral("Stopping recording..."), 5000);
    queueCv_.notify_all();

    // Watchdog independent of the worker: a synchronous WriteSample or
    // driver call can block without a deadline, and a blocked worker never
    // reaches FinalizeSegment. Without this timer the UI stayed on
    // "Finishing" forever (2026-07-30 field failure: 83-byte header-only
    // files, state log ending at Stopping).
    if (recordButton_) {
        const std::uint32_t generation = stopGeneration_.fetch_add(1) + 1;
        QTimer::singleShot(
            kStopWatchdogMs,
            recordButton_,
            [this, generation]() { OnStopWatchdogFired(generation); });
    }
}

void RecordingManager::OnStopWatchdogFired(std::uint32_t generation)
{
    if (generation != stopGeneration_.load()) {
        return; // A newer stop cycle owns the watchdog.
    }
    const RecordingState current = state_.load();
    if (current != RecordingState::Stopping &&
        current != RecordingState::Finalizing) {
        return; // The stop completed (Completed/Failed) in time.
    }
    AbandonWedgedWorker(
        QStringLiteral("stop watchdog, %1 ms").arg(kStopWatchdogMs));
}

void RecordingManager::OnWorkerHeartbeat()
{
    const RecordingState current = state_.load();
    const bool sessionLive = current == RecordingState::Starting ||
                             current == RecordingState::Recording ||
                             current == RecordingState::Stopping ||
                             current == RecordingState::Finalizing;
    if (!sessionLive || workerAbandoned_.load()) {
        if (heartbeatTimer_) {
            heartbeatTimer_->stop();
        }
        return;
    }
    const WorkerOp op = workerOp_.load(std::memory_order_relaxed);
    if (op == WorkerOp::kIdle) {
        return;
    }
    const std::int64_t startedMs =
        workerOpStartedMs_.load(std::memory_order_relaxed);
    const std::int64_t nowMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    if (startedMs > 0 && nowMs - startedMs >= kWorkerWedgeMs) {
        // Covers wedges the stop watchdog cannot see — for example a
        // mode-change finalize blocking mid-recording, which previously
        // dropped frames silently until the user happened to press Stop.
        AbandonWedgedWorker(
            QStringLiteral("heartbeat, single call over %1 ms")
                .arg(kWorkerWedgeMs));
    }
}

void RecordingManager::AbandonWedgedWorker(const QString& trigger)
{
    if (workerAbandoned_.exchange(true)) {
        return; // Already abandoned by the other watchdog.
    }
    // From here on, neither recorder's sink writer may ever be touched
    // again from any thread (Finalize concurrent with a wedged WriteSample
    // is forbidden); their teardown becomes a deliberate leak.
    processedRecorder_.MarkAbandoned();
    originalRecorder_.MarkAbandoned();
    const QString stage = DescribeWorkerStage();
    qCritical() << "Recording watchdog (" << trigger
                << ") declared the worker blocked" << stage
                << "- abandoning the worker; recording is disabled until "
                   "OkuFlow restarts.";
    RecordingSessionInfo endedSession;
    {
        std::lock_guard lock(queueMutex_);
        endedSession = sessionInfo_;
        frameQueue_.clear();
        audioQueue_.clear();
    }
    if (heartbeatTimer_) {
        heartbeatTimer_->stop();
    }
    SetState(RecordingState::Failed);
    UpdateButton();
    ShowStatus(
        QStringLiteral(
            "Recording could not continue: the video encoder stopped "
            "responding (%1). The files from this session may be "
            "incomplete. Recording is disabled until OkuFlow is "
            "restarted; everything else keeps working.")
            .arg(stage),
        20000);
    if (sessionEndedCallback_) {
        sessionEndedCallback_(endedSession);
    }
}

QString RecordingManager::DescribeWorkerStage() const
{
    const WorkerOp op = workerOp_.load(std::memory_order_relaxed);
    const std::int64_t startedMs =
        workerOpStartedMs_.load(std::memory_order_relaxed);
    const std::int64_t nowMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    QString description = QString::fromLatin1(WorkerOpName(op));
    if (op == WorkerOp::kProcessedVideo || op == WorkerOp::kOriginalVideo ||
        op == WorkerOp::kAudio || op == WorkerOp::kFinalize) {
        // Whichever recorder is mid-call reports a non-idle stage; audio and
        // finalize go through the same writers, so check both.
        const VideoRecorder::WriterStage processedStage =
            processedRecorder_.ActiveStage();
        const VideoRecorder::WriterStage active =
            processedStage != VideoRecorder::WriterStage::kIdle
                ? processedStage
                : originalRecorder_.ActiveStage();
        description += QStringLiteral(", %1")
                           .arg(QString::fromLatin1(
                               VideoRecorder::StageName(active)));
    }
    if (op != WorkerOp::kIdle && startedMs > 0 && nowMs >= startedMs) {
        description += QStringLiteral(" for %1 s")
                           .arg((nowMs - startedMs) / 1000);
    }
    description +=
        QStringLiteral(", codec %1, frame %2")
            .arg(CodecName())
            .arg(workerOpFrameSequence_.load(std::memory_order_relaxed));
    return description;
}

const char* RecordingManager::WorkerOpName(WorkerOp op)
{
    switch (op) {
    case WorkerOp::kIdle:
        return "between frames";
    case WorkerOp::kStartSegment:
        return "starting the encoders";
    case WorkerOp::kProcessedVideo:
        return "writing the processed video stream";
    case WorkerOp::kOriginalVideo:
        return "writing the original video stream";
    case WorkerOp::kAudio:
        return "writing the audio stream";
    case WorkerOp::kFinalize:
        return "finalizing the files";
    }
    return "in an unknown operation";
}

void RecordingManager::SetWorkerOp(WorkerOp op, std::uint64_t frameSequence)
{
    workerOp_.store(op, std::memory_order_relaxed);
    workerOpFrameSequence_.store(frameSequence, std::memory_order_relaxed);
    workerOpStartedMs_.store(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count(),
        std::memory_order_relaxed);
}

void RecordingManager::NotifyCaptureFrame(bool overwrotePendingPreview)
{
    if (!IsActive()) {
        return;
    }
    captureFramesSeen_.fetch_add(1);
    if (overwrotePendingPreview) {
        captureSlotOverwrite_.fetch_add(1);
    }
}

void RecordingManager::NotifyCaptureGpuRetry()
{
    if (IsActive()) {
        captureGpuRetry_.fetch_add(1);
    }
}

void RecordingManager::NotifyCaptureSafeCopyFallback()
{
    if (IsActive()) {
        captureSafeCopyFallback_.fetch_add(1);
    }
}

void RecordingManager::NotifyReadbackSkipped()
{
    if (IsActive()) {
        readbackRingBusy_.fetch_add(1);
    }
}

void RecordingManager::NotifyRecordingPoolSkipped()
{
    if (IsActive()) {
        recordingPoolBusy_.fetch_add(1);
    }
}

void RecordingManager::StorePendingOriginal(
    UINT64 requestId,
    CapturedFrame frame,
    const RecordingViewTransform& transform)
{
    if (!IsActive() || !frame.HasCpuPixels() || requestId == 0) {
        return;
    }
    std::lock_guard lock(pendingMutex_);
    pendingOriginalReadbacks_.insert_or_assign(
        requestId, PendingOriginal{std::move(frame), transform});
}

bool RecordingManager::HandleProcessedReadback(
    UINT64 requestId,
    const uint8_t* processedData,
    UINT processedWidth,
    UINT processedHeight)
{
    PendingOriginal pending;
    {
        std::lock_guard lock(pendingMutex_);
        auto original = pendingOriginalReadbacks_.find(requestId);
        if (original == pendingOriginalReadbacks_.end()) {
            return false;
        }

        pending = std::move(original->second);
        pendingOriginalReadbacks_.erase(original);
        for (auto stale = pendingOriginalReadbacks_.begin();
             stale != pendingOriginalReadbacks_.end();) {
            if (stale->first < requestId) {
                pairingMiss_.fetch_add(1);
                stale = pendingOriginalReadbacks_.erase(stale);
            } else {
                ++stale;
            }
        }
    }
    if (!IsActive()) {
        pairingMiss_.fetch_add(1);
        return false;
    }
    AddSceneFrame(processedData,
                  processedWidth,
                  processedHeight,
                  pending.transform,
                  std::move(pending.frame));
    return true;
}

void RecordingManager::ClearPendingReadbacks()
{
    std::lock_guard lock(pendingMutex_);
    const RecordingState state = state_.load();
    const bool sessionInFlight =
        state == RecordingState::Starting ||
        state == RecordingState::Recording ||
        state == RecordingState::Stopping ||
        state == RecordingState::Finalizing;
    if (sessionInFlight && !pendingOriginalReadbacks_.empty()) {
        pairingMiss_.fetch_add(
            static_cast<std::uint64_t>(pendingOriginalReadbacks_.size()));
    }
    pendingOriginalReadbacks_.clear();
}

void RecordingManager::AddSceneFrame(
    const uint8_t* processedSceneData,
    UINT processedSceneWidth,
    UINT processedSceneHeight,
    const RecordingViewTransform& transform,
    CapturedFrame originalFrame)
{
    if (!IsActive() || !processedSceneData ||
        processedSceneWidth == 0 || processedSceneHeight == 0 ||
        !originalFrame.HasCpuPixels() || !transform.valid) {
        return;
    }

    QueuedFrame queued;
    const std::size_t processedBytes =
        static_cast<std::size_t>(processedSceneWidth) *
        processedSceneHeight * 4u;
    queued.processedScene.assign(
        processedSceneData, processedSceneData + processedBytes);
    queued.processedSceneWidth = processedSceneWidth;
    queued.processedSceneHeight = processedSceneHeight;
    queued.transform = transform;
    queued.original = std::move(originalFrame);

    std::unique_lock lock(queueMutex_);
    if (stopRequested_ || shutdownRequested_) {
        return;
    }
    if (frameQueue_.size() >= kMaxQueuedFrames) {
        frameQueue_.pop_front();
        queueOverflow_.fetch_add(1);
    }
    frameQueue_.push_back(std::move(queued));
    lock.unlock();
    queueCv_.notify_all();
}

void RecordingManager::AddGpuSceneFrame(
    GpuVideoFrame processedScene,
    const RecordingViewTransform& transform,
    CapturedFrame originalFrame)
{
    if (!IsActive() || !processedScene.IsValid() ||
        !originalFrame.IsValid() || !transform.valid) {
        return;
    }

    QueuedFrame queued;
    queued.processedGpuScene = std::move(processedScene);
    queued.processedSceneWidth = queued.processedGpuScene.width;
    queued.processedSceneHeight = queued.processedGpuScene.height;
    queued.transform = transform;
    queued.original = std::move(originalFrame);

    std::unique_lock lock(queueMutex_);
    if (stopRequested_ || shutdownRequested_) {
        return;
    }
    if (frameQueue_.size() >= kMaxQueuedFrames) {
        frameQueue_.pop_front();
        queueOverflow_.fetch_add(1);
    }
    frameQueue_.push_back(std::move(queued));
    lock.unlock();
    queueCv_.notify_all();
}

QString RecordingManager::SegmentPath(bool processed) const
{
    const QString segment =
        segmentIndex_ <= 1
            ? QString()
            : QStringLiteral("_part%1").arg(segmentIndex_);
    return QDir(sessionDirectory_)
        .filePath(QStringLiteral("VID_%1%2_%3.mp4")
                      .arg(sessionTimestamp_,
                           segment,
                           processed ? QStringLiteral("processed")
                                     : QStringLiteral("original")));
}

bool RecordingManager::StartSegment(const QueuedFrame& firstFrame)
{
    if (workerAbandoned_.load()) {
        return false;
    }
    // Encoder creation (StartGpu/BeginWriting) is itself a set of unbounded
    // driver/MFT calls; track it so the heartbeat can catch an init wedge —
    // the first-frame scenario from the 2026-07-30 field failure.
    SetWorkerOp(WorkerOp::kStartSegment,
                firstFrame.original.identity.sequenceNumber);
    const RecordingCanvasSize canvas =
        ResolveRecordingCanvas(canvasMode_.load(),
                               firstFrame.original.width,
                               firstFrame.original.height);
    if (canvas.width == 0 || canvas.height == 0) {
        FinishSession(false, QStringLiteral("The recording canvas is invalid."));
        return false;
    }

    SegmentParameters parameters;
    parameters.processedWidth = canvas.width;
    parameters.processedHeight = canvas.height;
    parameters.originalWidth = firstFrame.original.width;
    parameters.originalHeight = firstFrame.original.height;
    parameters.frameRateNumerator = std::max(
        1u, firstFrame.original.identity.frameRateNumerator);
    parameters.frameRateDenominator = std::max(
        1u, firstFrame.original.identity.frameRateDenominator);
    const VideoRecorder::AudioFormat audioFormat{};
    const bool includeAudio = audioCaptureEnabled_;

    const QString processedPath = SegmentPath(true);
    const QString originalPath = SegmentPath(false);
    const std::array codecs{
        VideoRecorder::Codec::Av1, VideoRecorder::Codec::H264};
    QStringList failures;

    // Never overwrite or clean a pre-existing path, including a user-owned
    // zero-byte file. The paths below belong to this segment only after this.
    if (QFileInfo::exists(processedPath) || QFileInfo::exists(originalPath)) {
        FinishSession(false, QStringLiteral("Recording could not start."));
        return false;
    }

    for (const VideoRecorder::Codec codec : codecs) {
        processedRecorder_.Stop();
        originalRecorder_.Stop();
        RemoveEmptySessionRecording(processedPath);
        RemoveEmptySessionRecording(originalPath);
        // A failed initialization can already have written metadata or
        // fragments. Retain it instead of reusing the filename for a retry.
        if (QFileInfo::exists(processedPath) || QFileInfo::exists(originalPath)) {
            break;
        }

        const bool processedStarted =
            firstFrame.processedGpuScene.IsValid()
                ? processedRecorder_.StartGpu(
                      processedPath.toStdWString(),
                      parameters.processedWidth,
                      parameters.processedHeight,
                      parameters.frameRateNumerator,
                      parameters.frameRateDenominator,
                      codec,
                      firstFrame.processedGpuScene,
                      includeAudio ? &audioFormat : nullptr)
                : processedRecorder_.Start(
                      processedPath.toStdWString(),
                      parameters.processedWidth,
                      parameters.processedHeight,
                      parameters.frameRateNumerator,
                      parameters.frameRateDenominator,
                      codec,
                      includeAudio ? &audioFormat : nullptr);
        if (!processedStarted) {
            if (firstFrame.processedGpuScene.IsValid()) {
                gpuInputFallbacks_.fetch_add(1);
            }
            if (RecordingConsoleDiagnosticsEnabled()) {
                qWarning().noquote()
                    << QStringLiteral(
                           "Recording route attempt failed: segment %1, "
                           "processed, codec %2, requested %3: %4")
                           .arg(segmentIndex_)
                           .arg(QString::fromLatin1(
                               VideoRecorder::CodecName(codec)))
                           .arg(firstFrame.processedGpuScene.IsValid()
                                    ? QStringLiteral("GPU input")
                                    : QStringLiteral("CPU input"))
                           .arg(QString::fromStdString(
                               processedRecorder_.LastError()));
            }
            failures.append(
                QStringLiteral("%1 processed: %2")
                    .arg(QString::fromLatin1(
                             VideoRecorder::CodecName(codec)),
                         QString::fromStdString(
                             processedRecorder_.LastError())));
            continue;
        }
        const bool originalStarted =
            firstFrame.original.HasGpuScene()
                ? originalRecorder_.StartGpu(
                      originalPath.toStdWString(),
                      parameters.originalWidth,
                      parameters.originalHeight,
                      parameters.frameRateNumerator,
                      parameters.frameRateDenominator,
                      codec,
                      firstFrame.original.gpuScene,
                      includeAudio ? &audioFormat : nullptr)
                : originalRecorder_.Start(
                      originalPath.toStdWString(),
                      parameters.originalWidth,
                      parameters.originalHeight,
                      parameters.frameRateNumerator,
                      parameters.frameRateDenominator,
                      codec,
                      includeAudio ? &audioFormat : nullptr);
        if (!originalStarted) {
            if (firstFrame.original.HasGpuScene()) {
                gpuInputFallbacks_.fetch_add(1);
            }
            if (RecordingConsoleDiagnosticsEnabled()) {
                qWarning().noquote()
                    << QStringLiteral(
                           "Recording route attempt failed: segment %1, "
                           "original, codec %2, requested %3: %4")
                           .arg(segmentIndex_)
                           .arg(QString::fromLatin1(
                               VideoRecorder::CodecName(codec)))
                           .arg(firstFrame.original.HasGpuScene()
                                    ? QStringLiteral("GPU input")
                                    : QStringLiteral("CPU input"))
                           .arg(QString::fromStdString(
                               originalRecorder_.LastError()));
            }
            failures.append(
                QStringLiteral("%1 original: %2")
                    .arg(QString::fromLatin1(
                             VideoRecorder::CodecName(codec)),
                         QString::fromStdString(
                             originalRecorder_.LastError())));
            processedRecorder_.Stop();
            RemoveEmptySessionRecording(processedPath);
            continue;
        }

        processedWidth_ = parameters.processedWidth;
        segmentFinalized_ = false;
        processedHeight_ = parameters.processedHeight;
        originalWidth_ = parameters.originalWidth;
        originalHeight_ = parameters.originalHeight;
        frameRateNumerator_ = parameters.frameRateNumerator;
        frameRateDenominator_ = parameters.frameRateDenominator;
        segmentClockOrigin100ns_ =
            firstFrame.original.identity.captureClock100ns;
        const QString selectedCodec =
            QString::fromLatin1(VideoRecorder::CodecName(codec));
        {
            std::lock_guard lock(queueMutex_);
            codecName_ = selectedCodec;
        }
        SetState(RecordingState::Recording);
        PostButtonState(RecordingState::Recording);
        const bool processedGpuFed = processedRecorder_.UsesGpuInput();
        const bool originalGpuFed = originalRecorder_.UsesGpuInput();
        const bool processedWorkerReadback =
            processedRecorder_.UsesWorkerGpuReadback();
        const bool originalWorkerReadback =
            originalRecorder_.UsesWorkerGpuReadback();
        const QString feed =
            processedGpuFed && originalGpuFed
                ? QStringLiteral("GPU-fed original and processed")
                : ((processedGpuFed || originalGpuFed)
                       ? QStringLiteral("original and processed (partially GPU-fed)")
                       : ((processedWorkerReadback || originalWorkerReadback)
                              ? QStringLiteral("original and processed (worker-readback)")
                              : QStringLiteral("original and processed")));
        if (processedRecorder_.UsesWorkerGpuReadback()) {
            gpuInputFallbacks_.fetch_add(1);
        }
        if (originalRecorder_.UsesWorkerGpuReadback()) {
            gpuInputFallbacks_.fetch_add(1);
        }
        if (RecordingConsoleDiagnosticsEnabled()) {
            const QString diagnostic =
                QStringLiteral(
                    "Recording route: segment %1 | codec %2 | "
                    "processed=%3 (%4x%5, \"%6\") | "
                    "original=%7 (%8x%9, \"%10\") | audio=%11")
                    .arg(segmentIndex_)
                    .arg(selectedCodec)
                    .arg(RecordingInputRoute(processedRecorder_))
                    .arg(processedWidth_)
                    .arg(processedHeight_)
                    .arg(QDir::toNativeSeparators(processedPath))
                    .arg(RecordingInputRoute(originalRecorder_))
                    .arg(originalWidth_)
                    .arg(originalHeight_)
                    .arg(QDir::toNativeSeparators(originalPath))
                    .arg(includeAudio ? QStringLiteral("microphone/AAC")
                                      : QStringLiteral("off"));
            if (processedGpuFed && originalGpuFed) {
                qInfo().noquote() << diagnostic;
            } else {
                qWarning().noquote() << diagnostic;
            }
        }
        PostStatus(
            QStringLiteral(
                "Recording %1 video%2 with %3 at "
                "%4/%5 FPS. Press Ctrl+Shift+O to open the OkuFlow folder.")
                .arg(feed)
                .arg(includeAudio ? QStringLiteral(" with microphone audio")
                                  : QString())
                .arg(selectedCodec)
                .arg(frameRateNumerator_)
                .arg(frameRateDenominator_),
            5000);
        return true;
    }

    processedRecorder_.Stop();
    originalRecorder_.Stop();
    RemoveEmptySessionRecording(processedPath);
    RemoveEmptySessionRecording(originalPath);
    FinishSession(
        false,
        failures.isEmpty()
            ? QStringLiteral("Recording could not start.")
            : QStringLiteral("Recording could not start: %1")
                  .arg(failures.constLast()));
    return false;
}

bool RecordingManager::WriteFrame(QueuedFrame&& frame)
{
    const UINT incomingNumerator =
        std::max(1u, frame.original.identity.frameRateNumerator);
    const UINT incomingDenominator =
        std::max(1u, frame.original.identity.frameRateDenominator);
    const bool modeChanged =
        originalRecorder_.IsRecording() &&
        (frame.original.width != originalWidth_ ||
         frame.original.height != originalHeight_ ||
         incomingNumerator != frameRateNumerator_ ||
         incomingDenominator != frameRateDenominator_);
    if (modeChanged) {
        if (!FinalizeSegment(false)) {
            FinishSession(false);
            return false;
        }
        {
            std::lock_guard lock(queueMutex_);
            if (stopRequested_ || shutdownRequested_) {
                return true;
            }
        }
        ++segmentIndex_;
        PostStatus(
            QStringLiteral(
                "The camera mode changed. Recording continues in part %1.")
                .arg(segmentIndex_),
            6000);
        if (!StartSegment(frame)) {
            return false;
        }
    } else if (!processedRecorder_.IsRecording() &&
               !StartSegment(frame)) {
        return false;
    }

    if (frame.original.identity.captureTimestamp100ns < 0) {
        unknownTimestamp_.fetch_add(1);
    }
    RecordingFrameIdentity synchronizedIdentity = frame.original.identity;
    if (synchronizedIdentity.captureClock100ns >= 0) {
        synchronizedIdentity.captureTimestamp100ns =
            synchronizedIdentity.captureClock100ns;
    }
    const auto encoderSubmitStarted = std::chrono::steady_clock::now();
    SetWorkerOp(WorkerOp::kProcessedVideo,
                synchronizedIdentity.sequenceNumber);
    bool processedWritten = false;
    if (frame.processedGpuScene.IsValid() &&
        (processedRecorder_.UsesGpuInput() ||
         processedRecorder_.UsesWorkerGpuReadback())) {
        processedWritten = processedRecorder_.AddGpuFrame(
            frame.processedGpuScene, synchronizedIdentity);
        if (processedWritten && processedRecorder_.UsesGpuInput()) {
            gpuFramesWritten_.fetch_add(1);
        }
    } else {
        if (processedRecorder_.UsesGpuInput() ||
            processedRecorder_.UsesWorkerGpuReadback()) {
            const std::uint64_t fallback =
                processedFrameFallbacks_.fetch_add(1) + 1;
            if (RecordingConsoleDiagnosticsEnabled() &&
                (fallback == 1 || fallback % 30 == 0)) {
                qWarning()
                    << "Processed recording frame lost its GPU surface;"
                    << "using the CPU frame for this sample. Count:"
                    << fallback;
            }
        }
        std::vector<std::uint8_t> processedCanvas;
        if (!ResampleRecordingCanvas(
                frame.processedScene.data(),
                frame.processedSceneWidth,
                frame.processedSceneHeight,
                frame.transform,
                processedWidth_,
                processedHeight_,
                processedCanvas)) {
            encoderReject_.fetch_add(1);
            FinishSession(
                false,
                QStringLiteral(
                    "Recording stopped because the processed frame could not "
                    "be mapped to the recording canvas."));
            return false;
        }
        processedWritten = processedRecorder_.AddFrame(
            processedCanvas.data(),
            static_cast<std::size_t>(processedWidth_) * 4u,
            synchronizedIdentity);
    }
    SetWorkerOp(WorkerOp::kOriginalVideo,
                synchronizedIdentity.sequenceNumber);
    bool originalWritten = false;
    if (frame.original.HasGpuScene() &&
        (originalRecorder_.UsesGpuInput() ||
         originalRecorder_.UsesWorkerGpuReadback())) {
        originalWritten = originalRecorder_.AddGpuFrame(
            frame.original.gpuScene, synchronizedIdentity);
        if (originalWritten && originalRecorder_.UsesGpuInput()) {
            gpuFramesWritten_.fetch_add(1);
        }
    } else if (frame.original.HasCpuPixels()) {
        if (originalRecorder_.UsesGpuInput() ||
            originalRecorder_.UsesWorkerGpuReadback()) {
            const std::uint64_t fallback =
                originalFrameFallbacks_.fetch_add(1) + 1;
            if (RecordingConsoleDiagnosticsEnabled() &&
                (fallback == 1 || fallback % 30 == 0)) {
                qWarning()
                    << "Original recording frame lost its GPU surface;"
                    << "using the CPU frame for this sample. Count:"
                    << fallback;
            }
        }
        originalWritten = originalRecorder_.AddFrame(
            frame.original.pixels.data(),
            static_cast<std::size_t>(frame.original.width) * 4u,
            synchronizedIdentity);
    }
    const float encoderSubmitMs =
        std::chrono::duration<float, std::milli>(
            std::chrono::steady_clock::now() - encoderSubmitStarted)
            .count();
    RecordEncoderSubmitTiming(encoderSubmitMs);
    if (!processedWritten || !originalWritten) {
        encoderReject_.fetch_add(1);
        const VideoRecorder& failed =
            processedWritten ? originalRecorder_ : processedRecorder_;
        const QString detail =
            QString::fromStdString(failed.LastError());
        FinishSession(
            false,
            detail.isEmpty()
                ? QStringLiteral("Recording stopped because an encoder rejected a frame.")
                : detail);
        return false;
    }

    framesWritten_.fetch_add(1);
    constexpr double kMaxSeconds = 12.0 * 3600.0;
    if (processedRecorder_.DurationSeconds() >= kMaxSeconds) {
        {
            std::lock_guard lock(queueMutex_);
            stopRequested_ = true;
            stopMessage_ =
                QStringLiteral(
                    "Recording stopped after reaching the 12-hour limit.");
        }
        SetState(RecordingState::Stopping);
    }
    return true;
}

bool RecordingManager::WriteAudioFrame(AudioFrame&& frame)
{
    if (!audioCaptureEnabled_ || segmentClockOrigin100ns_ < 0 ||
        frame.captureClock100ns < segmentClockOrigin100ns_) {
        return true;
    }
    if (!processedRecorder_.IsRecording() ||
        !originalRecorder_.IsRecording()) {
        return true;
    }

    const std::int64_t sampleTime100ns =
        frame.captureClock100ns - segmentClockOrigin100ns_;
    SetWorkerOp(WorkerOp::kAudio,
                workerOpFrameSequence_.load(std::memory_order_relaxed));
    const bool processedWritten = processedRecorder_.AddAudioFrame(
        frame.pcm.data(), frame.pcm.size(), sampleTime100ns,
        frame.duration100ns);
    const bool originalWritten = originalRecorder_.AddAudioFrame(
        frame.pcm.data(), frame.pcm.size(), sampleTime100ns,
        frame.duration100ns);
    if (processedWritten && originalWritten) {
        return true;
    }

    encoderReject_.fetch_add(1);
    const VideoRecorder& failed =
        processedWritten ? originalRecorder_ : processedRecorder_;
    const QString detail = QString::fromStdString(failed.LastError());
    FinishSession(
        false,
        detail.isEmpty()
            ? QStringLiteral(
                  "Recording stopped because the audio encoder rejected a sample.")
            : detail);
    return false;
}

bool RecordingManager::FinalizeSegment(bool terminal)
{
    if (segmentFinalized_) {
        return true;
    }
    if (terminal) {
        SetState(RecordingState::Finalizing);
        PostButtonState(RecordingState::Finalizing);
        // Truthful UI: "Finishing" appears only now that finalization has
        // genuinely begun; Stop() itself reports "Stopping".
        PostStatus(QStringLiteral("Finishing recording..."), 5000);
    }
    SetWorkerOp(WorkerOp::kFinalize,
                workerOpFrameSequence_.load(std::memory_order_relaxed));

    const QString processedPath = SegmentPath(true);
    const QString originalPath = SegmentPath(false);
    const QString processedRoute =
        RecordingInputRoute(processedRecorder_);
    const QString originalRoute =
        RecordingInputRoute(originalRecorder_);
    const VideoRecorder::FinalizeResult processed =
        processedRecorder_.Stop();
    const VideoRecorder::FinalizeResult original =
        originalRecorder_.Stop();
    segmentFinalized_ = true;
    if (RecordingConsoleDiagnosticsEnabled()) {
        qInfo().noquote()
            << QStringLiteral(
                   "Recording segment result: segment %1 | "
                   "processed=%2, %3 samples, %4, \"%5\" | "
                   "original=%6, %7 samples, %8, \"%9\"")
                   .arg(segmentIndex_)
                   .arg(processedRoute)
                   .arg(processed.videoSamplesWritten)
                   .arg(QString::fromLatin1(
                       FinalizeDispositionName(processed.disposition)))
                   .arg(QDir::toNativeSeparators(processedPath))
                   .arg(originalRoute)
                   .arg(original.videoSamplesWritten)
                   .arg(QString::fromLatin1(
                       FinalizeDispositionName(original.disposition)))
                   .arg(QDir::toNativeSeparators(originalPath));
    }
    const bool processedHasSamples =
        processed.videoSamplesWritten > 0;
    const bool originalHasSamples =
        original.videoSamplesWritten > 0;
    RemoveEmptySessionRecording(processedPath, processed.videoSamplesWritten);
    RemoveEmptySessionRecording(originalPath, original.videoSamplesWritten);
    sessionHadOutput_ = sessionHadOutput_ || processedHasSamples ||
        originalHasSamples;
    if (!processedHasSamples && !originalHasSamples) {
        if (processed.FullyCompleted() && original.FullyCompleted()) {
            return true;
        }
        sessionFinalizationFailed_ = true;
        return false;
    }
    if (processedHasSamples != originalHasSamples) {
        sessionFinalizationFailed_ = true;
        PostStatus(
            QStringLiteral(
                "Recording stopped because the original and processed "
                "streams did not form a complete pair."),
            12000);
        return false;
    }
    sessionHadOutput_ = true;
    const QString processedFailure =
        FinalizeFailureText(QStringLiteral("Processed video"), processed);
    const QString originalFailure =
        FinalizeFailureText(QStringLiteral("Original video"), original);
    if (!processedFailure.isEmpty() || !originalFailure.isEmpty()) {
        sessionFinalizationFailed_ = true;
        QStringList details;
        if (!processedFailure.isEmpty()) {
            details.append(processedFailure);
        }
        if (!originalFailure.isEmpty()) {
            details.append(originalFailure);
        }
        PostStatus(details.join(QLatin1Char(' ')), 12000);
        return false;
    }
    if (processed.HasPlayableVideo() && original.HasPlayableVideo() &&
        QFileInfo::exists(processedPath) && QFileInfo::exists(originalPath)) {
        PostSegmentSaved(originalPath, processedPath);
    }
    return true;
}

RecordingDropCounts RecordingManager::DropCounts() const
{
    return RecordingDropCounts{
        captureSlotOverwrite_.load(),
        readbackRingBusy_.load(),
        recordingPoolBusy_.load(),
        pairingMiss_.load(),
        queueOverflow_.load(),
        encoderReject_.load(),
        unknownTimestamp_.load(),
    };
}

QString RecordingManager::BuildDropSummary() const
{
    const RecordingDropCounts drops = DropCounts();
    if (drops.Total() == 0 && drops.unknownTimestamp == 0) {
        return QStringLiteral("No frames were dropped.");
    }

    QStringList causes;
    if (drops.captureSlotOverwrite > 0) {
        causes.append(
            QStringLiteral("%1 capture-slot overwrite")
                .arg(drops.captureSlotOverwrite));
    }
    if (drops.readbackRingBusy > 0) {
        causes.append(
            QStringLiteral("%1 display/readback busy")
                .arg(drops.readbackRingBusy));
    }
    if (drops.recordingPoolBusy > 0) {
        causes.append(
            QStringLiteral("%1 recording-texture pool busy")
                .arg(drops.recordingPoolBusy));
    }
    if (drops.pairingMiss > 0) {
        causes.append(
            QStringLiteral("%1 unmatched readback")
                .arg(drops.pairingMiss));
    }
    if (drops.queueOverflow > 0) {
        causes.append(
            QStringLiteral("%1 recording-queue overflow")
                .arg(drops.queueOverflow));
    }
    if (drops.encoderReject > 0) {
        causes.append(
            QStringLiteral("%1 encoder rejection")
                .arg(drops.encoderReject));
    }
    QString summary =
        QStringLiteral("%1 frames were dropped (%2).")
            .arg(drops.Total())
            .arg(causes.join(QStringLiteral(", ")));
    if (drops.unknownTimestamp > 0) {
        summary +=
            QStringLiteral(" %1 frames used an estimated timestamp.")
                .arg(drops.unknownTimestamp);
    }
    return summary;
}

void RecordingManager::FinishSession(bool success, const QString& detail)
{
    if (workerAbandoned_.load()) {
        // The stop watchdog already declared this session dead and restored
        // the UI. If the worker eventually un-wedges, do not let it post a
        // stale "Recording saved" over the failure report.
        qWarning() << "Recording worker resumed after the stop watchdog "
                      "abandoned the session; suppressing its completion.";
        std::lock_guard lock(queueMutex_);
        frameQueue_.clear();
        audioQueue_.clear();
        stopRequested_ = false;
        return;
    }
    if (!success) {
        qWarning() << "Recording session failed:"
                   << (detail.isEmpty()
                           ? QStringLiteral("no encoder detail was reported")
                           : detail);
    }
    ClearPendingReadbacks();
    bool finalized = true;
    if (!segmentFinalized_) {
        finalized = FinalizeSegment(true);
    }
    const RecordingCompletionOutcome outcome =
        ClassifyRecordingCompletion(success,
                                    sessionHadOutput_,
                                    finalized,
                                    sessionFinalizationFailed_);
    const bool saved =
        outcome == RecordingCompletionOutcome::Saved;

    const QString drops = BuildDropSummary();
    if (RecordingConsoleDiagnosticsEnabled()) {
        qInfo().noquote()
            << QStringLiteral(
                   "Recording session diagnostics: paired frames=%1 | "
                   "direct-GPU stream samples=%2 | "
                   "GPU-route degradations=%3 | "
                   "processed per-frame CPU fallbacks=%4 | "
                   "original per-frame CPU fallbacks=%5 | "
                   "capture GPU-completion retries=%6 | "
                   "capture safe-copy frames=%7 | %8")
                   .arg(framesWritten_.load())
                   .arg(gpuFramesWritten_.load())
                   .arg(gpuInputFallbacks_.load())
                   .arg(processedFrameFallbacks_.load())
                   .arg(originalFrameFallbacks_.load())
                   .arg(captureGpuRetry_.load())
                   .arg(captureSafeCopyFallback_.load())
                   .arg(drops);
    }
    QString message;
    if (outcome == RecordingCompletionOutcome::Saved) {
        message =
            stopMessage_.isEmpty()
                ? QStringLiteral("Recording saved. %1").arg(drops)
                : QStringLiteral("%1 Recording saved. %2")
                      .arg(stopMessage_, drops);
        SetState(RecordingState::Completed);
    } else if (outcome ==
               RecordingCompletionOutcome::FailedBeforeFirstFrame) {
        message =
            detail.isEmpty()
                ? QStringLiteral(
                      "Recording stopped before the first frame was written.")
                : detail;
        SetState(RecordingState::Failed);
    } else {
        message =
            detail.isEmpty()
                ? QStringLiteral(
                      "Recording ended with an error. Earlier completed "
                      "fragments may still be playable. %1")
                      .arg(drops)
                : QStringLiteral("%1 %2").arg(detail, drops);
        SetState(RecordingState::Failed);
    }
    PostButtonState(state_.load());
    PostStatus(message, saved ? 9000 : 14000);

    RecordingSessionInfo endedSession;
    {
        std::lock_guard lock(queueMutex_);
        endedSession = sessionInfo_;
        frameQueue_.clear();
        audioQueue_.clear();
        stopRequested_ = false;
        stopMessage_.clear();
    }
    if (sessionEndedCallback_ && recordButton_ &&
        !workerAbandoned_.load()) {
        const SessionEndedCallback callback = sessionEndedCallback_;
        QMetaObject::invokeMethod(
            recordButton_,
            [callback, endedSession]() { callback(endedSession); },
            Qt::QueuedConnection);
    }
}

void RecordingManager::WorkerLoop()
{
    const HRESULT coResult =
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitializeCom = SUCCEEDED(coResult);

    while (true) {
        QueuedFrame frame;
        AudioFrame audio;
        bool haveFrame = false;
        bool haveAudio = false;
        bool shouldStop = false;
        bool shouldShutdown = false;
        {
            std::unique_lock lock(queueMutex_);
            queueCv_.wait(lock, [this]() {
                return shutdownRequested_ || stopRequested_ ||
                       !frameQueue_.empty() ||
                       (!audioQueue_.empty() &&
                        processedRecorder_.IsRecording() &&
                        originalRecorder_.IsRecording());
            });
            shouldShutdown = shutdownRequested_;
            const bool recorderReady =
                processedRecorder_.IsRecording() &&
                originalRecorder_.IsRecording();
            const bool audioComesFirst =
                !stopRequested_ && recorderReady && !audioQueue_.empty() &&
                (frameQueue_.empty() ||
                 audioQueue_.front().captureClock100ns <=
                     frameQueue_.front()
                         .original.identity.captureClock100ns);
            if (audioComesFirst && !shouldShutdown) {
                audio = std::move(audioQueue_.front());
                audioQueue_.pop_front();
                workerWriting_ = true;
                haveAudio = true;
            } else if (!stopRequested_ && !frameQueue_.empty() &&
                       !shouldShutdown) {
                frame = std::move(frameQueue_.front());
                frameQueue_.pop_front();
                workerWriting_ = true;
                haveFrame = true;
            } else {
                shouldStop = stopRequested_;
            }
        }
        queueCv_.notify_all();

        if (haveFrame) {
            const bool keepGoing = WriteFrame(std::move(frame));
            SetWorkerOp(WorkerOp::kIdle, 0);
            {
                std::lock_guard lock(queueMutex_);
                workerWriting_ = false;
            }
            queueCv_.notify_all();
            if (!keepGoing) {
                std::lock_guard lock(queueMutex_);
                frameQueue_.clear();
                stopRequested_ = false;
            }
            continue;
        }
        if (haveAudio) {
            const bool keepGoing = WriteAudioFrame(std::move(audio));
            SetWorkerOp(WorkerOp::kIdle, 0);
            {
                std::lock_guard lock(queueMutex_);
                workerWriting_ = false;
            }
            queueCv_.notify_all();
            if (!keepGoing) {
                std::lock_guard lock(queueMutex_);
                frameQueue_.clear();
                audioQueue_.clear();
                stopRequested_ = false;
            }
            continue;
        }

        if (shouldShutdown) {
            if (!segmentFinalized_) {
                FinalizeSegment(false);
            }
            break;
        }
        if (shouldStop) {
            FinishSession(true);
            SetWorkerOp(WorkerOp::kIdle, 0);
        }
    }

    if (uninitializeCom) {
        CoUninitialize();
    }
    // Lets the destructor's bounded wait distinguish "worker exited" from
    // "worker is wedged inside a driver call" without an unbounded join.
    {
        std::lock_guard lock(queueMutex_);
        workerDone_ = true;
    }
    queueCv_.notify_all();
}

void RecordingManager::PostStatus(
    const QString& message,
    int durationMs) const
{
    if (workerAbandoned_.load() || !recordButton_ || !statusCallback_ ||
        message.isEmpty()) {
        // After abandonment the worker must stay silent: the manager may be
        // leaked past UI teardown, so recordButton_ can dangle.
        return;
    }
    const QPointer<QPushButton> context(recordButton_);
    const StatusCallback callback = statusCallback_;
    QMetaObject::invokeMethod(
        recordButton_,
        [context, callback, message, durationMs]() {
            if (context && callback) {
                callback(message, durationMs);
            }
        },
        Qt::QueuedConnection);
}

void RecordingManager::PostButtonState(RecordingState state) const
{
    if (workerAbandoned_.load() || !recordButton_) {
        return;
    }
    const QPointer<QPushButton> button(recordButton_);
    QMetaObject::invokeMethod(
        recordButton_,
        [button, state]() {
            if (!button) {
                return;
            }
            QSignalBlocker blocker(button);
            const bool active =
                state == RecordingState::Starting ||
                state == RecordingState::Recording;
            const bool finalizing =
                state == RecordingState::Stopping ||
                state == RecordingState::Finalizing;
            button->setChecked(active);
            button->setEnabled(!finalizing);
            SetLiveText(
                button,
                finalizing
                    ? QStringLiteral("Finishing")
                    : active ? QStringLiteral("Stop")
                             : QStringLiteral("Record"),
                LivePoliteness::kSilent,
                QStringLiteral("Record"));
        },
        Qt::QueuedConnection);
}

void RecordingManager::PostSegmentSaved(
    const QString& originalPath,
    const QString& processedPath) const
{
    if (workerAbandoned_.load() || !recordButton_ ||
        !segmentSavedCallback_ ||
        originalPath.isEmpty() || processedPath.isEmpty()) {
        return;
    }
    SavedRecordingSegment saved;
    {
        std::lock_guard lock(queueMutex_);
        saved.session = sessionInfo_;
        saved.segmentIndex = segmentIndex_;
    }
    saved.originalPath = originalPath;
    saved.processedPath = processedPath;
    const QPointer<QPushButton> context(recordButton_);
    const SegmentSavedCallback callback = segmentSavedCallback_;
    QMetaObject::invokeMethod(
        recordButton_,
        [context, callback, saved]() {
            if (context && callback) {
                callback(saved);
            }
        },
        Qt::QueuedConnection);
}

QString RecordingManager::EnsureOutputDirectory() const
{
    QString error;
    const QString outputPath =
        userDataPaths_
            ? userDataPaths_->RecordingsForDate(QDate::currentDate(), &error)
            : QString();
    if (!error.isEmpty()) {
        qWarning() << error;
    }
    return outputPath;
}

void RecordingManager::ShowStatus(
    const QString& message,
    int durationMs) const
{
    if (statusCallback_ && !message.isEmpty()) {
        statusCallback_(message, durationMs);
    }
}

void RecordingManager::UpdateButton()
{
    if (!recordButton_) {
        return;
    }
    const RecordingState state = state_.load();
    const bool active =
        state == RecordingState::Starting ||
        state == RecordingState::Recording;
    const bool finalizing =
        state == RecordingState::Stopping ||
        state == RecordingState::Finalizing;
    QSignalBlocker blocker(recordButton_);
    recordButton_->setChecked(active);
    recordButton_->setEnabled(!finalizing);
    SetLiveText(
        recordButton_,
        finalizing
            ? QStringLiteral("Finishing")
            : active ? QStringLiteral("Stop")
                     : QStringLiteral("Record"),
        LivePoliteness::kSilent,
        QStringLiteral("Record"));
}

} // namespace okuflow

#endif // _WIN32

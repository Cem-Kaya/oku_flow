#ifdef _WIN32

#include "okuflow/app/pipeline_orchestrator.hpp"

#include <QDebug>
#include <QMetaObject>

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace okuflow {

namespace {

template <std::size_t Capacity>
TimingPercentiles CalculatePercentiles(
    const std::array<float, Capacity>& samples,
    std::size_t sampleCount)
{
    TimingPercentiles result;
    result.sampleCount = std::min(sampleCount, Capacity);
    if (result.sampleCount == 0) {
        return result;
    }

    std::vector<float> sorted(samples.begin(),
                              samples.begin() + result.sampleCount);
    std::sort(sorted.begin(), sorted.end());
    const auto percentile = [&sorted](float fraction) {
        const std::size_t index = static_cast<std::size_t>(
            std::ceil(fraction * static_cast<float>(sorted.size())) - 1.0f);
        return sorted[std::min(index, sorted.size() - 1)];
    };
    result.p50Ms = percentile(0.50f);
    result.p95Ms = percentile(0.95f);
    result.p99Ms = percentile(0.99f);
    return result;
}

} // namespace

PipelineOrchestrator::PipelineOrchestrator(QObject& context,
                                           Callbacks callbacks)
    : callbacks_(std::move(callbacks))
{
    timer_.setTimerType(Qt::PreciseTimer);
    QObject::connect(&timer_, &QTimer::timeout, &context,
                     [this]() { OnTick(); });
}

PipelineOrchestrator::~PipelineOrchestrator()
{
    Stop();
}

void PipelineOrchestrator::Start()
{
    running_.store(true, std::memory_order_release);
    viewportTickTimer_.start();
    viewportMotionTailTimer_.start();
    viewportRateMeasurementTimer_.start();
    UpdateTimerPolicy();
    timer_.start();
}

void PipelineOrchestrator::Stop()
{
    running_.store(false, std::memory_order_release);
    cameraTickQueued_.store(false, std::memory_order_release);
    timer_.stop();
}

void PipelineOrchestrator::NotifyCameraFrameAvailable(int delayMs)
{
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }
    bool expected = false;
    if (!cameraTickQueued_.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
        return;
    }

    const auto dispatch = [this]() {
        cameraTickQueued_.store(false, std::memory_order_release);
        if (running_.load(std::memory_order_acquire)) {
            OnTick();
        }
    };
    if (delayMs > 0) {
        QTimer::singleShot(delayMs, &timer_, dispatch);
    } else {
        QMetaObject::invokeMethod(
            &timer_, dispatch, Qt::QueuedConnection);
    }
}

void PipelineOrchestrator::SetViewportRateMode(
    settings::ViewportRateMode mode)
{
    viewportRateMode_ = mode;
    NotifyViewportMotion();
    UpdateTimerPolicy();
}

settings::ViewportRateMode PipelineOrchestrator::ViewportRateMode() const
{
    return viewportRateMode_;
}

void PipelineOrchestrator::SetViewportFitMode(
    settings::ViewportFitModeSetting mode)
{
    viewportFitMode_ = mode;
    MarkViewportDirty();
}

settings::ViewportFitModeSetting PipelineOrchestrator::ViewportFitMode() const
{
    return viewportFitMode_;
}

void PipelineOrchestrator::MarkViewportDirty()
{
    viewportDirty_ = true;
    NotifyViewportMotion();
    UpdateTimerPolicy();
}

bool PipelineOrchestrator::IsViewportDirty() const
{
    return viewportDirty_;
}

void PipelineOrchestrator::NotifyViewportMotion()
{
    viewportMotionTailTimer_.restart();
}

void PipelineOrchestrator::MarkViewportPresented()
{
    viewportDirty_ = false;
    ++viewportPresentCount_;
    if (viewportRateMeasurementTimer_.elapsed() < 1000) {
        return;
    }

    const qint64 elapsedMs = viewportRateMeasurementTimer_.restart();
    measuredViewportRate_ =
        elapsedMs > 0
            ? static_cast<float>(viewportPresentCount_) * 1000.0f /
                  static_cast<float>(elapsedMs)
            : 0.0f;
    viewportPresentCount_ = 0;
    if (callbacks_.queryDisplayRefreshRate) {
        displayRefreshHz_ =
            std::clamp(callbacks_.queryDisplayRefreshRate(), 24, 500);
    }
}

int PipelineOrchestrator::EffectiveViewportRate() const
{
    const int displayRate = std::clamp(displayRefreshHz_, 24, 500);
    switch (viewportRateMode_) {
    case settings::ViewportRateMode::Fps60:
        return std::min(displayRate, 60);
    case settings::ViewportRateMode::Fps90:
        return std::min(displayRate, 90);
    case settings::ViewportRateMode::Fps120:
        return std::min(displayRate, 120);
    case settings::ViewportRateMode::MatchDisplay:
        return displayRate;
    case settings::ViewportRateMode::AutoUpTo120:
    default:
        return std::min(displayRate, 120);
    }
}

int PipelineOrchestrator::RequestedViewportRate() const
{
    switch (viewportRateMode_) {
    case settings::ViewportRateMode::Fps60:
        return 60;
    case settings::ViewportRateMode::Fps90:
        return 90;
    case settings::ViewportRateMode::Fps120:
        return 120;
    case settings::ViewportRateMode::MatchDisplay:
        return displayRefreshHz_;
    case settings::ViewportRateMode::AutoUpTo120:
    default:
        return std::min(displayRefreshHz_, 120);
    }
}

int PipelineOrchestrator::DisplayRefreshRate() const
{
    return displayRefreshHz_;
}

float PipelineOrchestrator::MeasuredViewportRate() const
{
    return measuredViewportRate_;
}

float PipelineOrchestrator::FrameTickAverageMs() const
{
    return frameTickAverageMs_;
}

TimingPercentiles PipelineOrchestrator::FrameTickPercentiles() const
{
    return CalculatePercentiles(frameTickSamplesMs_, frameTickSampleCount_);
}

void PipelineOrchestrator::RecordCaptureToPresentSample(float milliseconds)
{
    if (!std::isfinite(milliseconds) || milliseconds < 0.0f) {
        return;
    }
    captureToPresentSamplesMs_[captureToPresentSampleIndex_] = milliseconds;
    captureToPresentSampleIndex_ =
        (captureToPresentSampleIndex_ + 1) %
        captureToPresentSamplesMs_.size();
    captureToPresentSampleCount_ =
        std::min(captureToPresentSampleCount_ + 1,
                 captureToPresentSamplesMs_.size());
}

TimingPercentiles PipelineOrchestrator::CaptureToPresentPercentiles() const
{
    return CalculatePercentiles(captureToPresentSamplesMs_,
                                captureToPresentSampleCount_);
}

void PipelineOrchestrator::RecordStageSample(FrameTimingStage stage,
                                             float milliseconds)
{
    if (!std::isfinite(milliseconds) || milliseconds < 0.0f) {
        return;
    }
    const std::size_t stageIndex = static_cast<std::size_t>(stage);
    if (stageIndex >= kFrameTimingStageCount) {
        return;
    }
    auto& samples = stageSamplesMs_[stageIndex];
    std::size_t& sampleIndex = stageSampleIndexes_[stageIndex];
    std::size_t& sampleCount = stageSampleCounts_[stageIndex];
    samples[sampleIndex] = milliseconds;
    sampleIndex = (sampleIndex + 1) % samples.size();
    sampleCount = std::min(sampleCount + 1, samples.size());
}

TimingPercentiles PipelineOrchestrator::StagePercentiles(
    FrameTimingStage stage) const
{
    const std::size_t stageIndex = static_cast<std::size_t>(stage);
    if (stageIndex >= kFrameTimingStageCount) {
        return {};
    }
    return CalculatePercentiles(stageSamplesMs_[stageIndex],
                                stageSampleCounts_[stageIndex]);
}

bool PipelineOrchestrator::BeginCameraReconnect(qint64 nowMs)
{
    if (cameraReconnectPending_) {
        return false;
    }
    cameraReconnectPending_ = true;
    cameraReconnectAttempt_ = 0;
    cameraReconnectStartedMs_ = nowMs;
    cameraReconnectNextAttemptMs_ = nowMs + 2000;
    return true;
}

void PipelineOrchestrator::CancelCameraReconnect()
{
    cameraReconnectPending_ = false;
    cameraReconnectAttempt_ = 0;
    cameraReconnectStartedMs_ = 0;
    cameraReconnectNextAttemptMs_ = 0;
}

bool PipelineOrchestrator::IsCameraReconnectPending() const
{
    return cameraReconnectPending_;
}

bool PipelineOrchestrator::CameraReconnectDue(qint64 nowMs) const
{
    return cameraReconnectPending_ &&
           nowMs >= cameraReconnectNextAttemptMs_;
}

bool PipelineOrchestrator::CameraReconnectExpired(qint64 nowMs) const
{
    constexpr qint64 kReconnectWindowMs = 30000;
    return cameraReconnectPending_ &&
           nowMs - cameraReconnectStartedMs_ >= kReconnectWindowMs;
}

int PipelineOrchestrator::CameraReconnectAttempt() const
{
    return cameraReconnectAttempt_;
}

void PipelineOrchestrator::ScheduleCameraReconnectRetry(qint64 nowMs)
{
    ++cameraReconnectAttempt_;
    const qint64 delayMs =
        std::min<qint64>(
            2000ll << std::min(cameraReconnectAttempt_, 2), 8000ll);
    cameraReconnectNextAttemptMs_ = nowMs + delayMs;
}

bool PipelineOrchestrator::FenceInteropEnabled() const
{
    return fenceInteropEnabled_;
}

void PipelineOrchestrator::SetFenceInteropEnabled(bool enabled)
{
    fenceInteropEnabled_ = enabled;
}

int PipelineOrchestrator::RecordCudaFailure()
{
    return ++consecutiveCudaFailures_;
}

void PipelineOrchestrator::ResetCudaFailures()
{
    consecutiveCudaFailures_ = 0;
}

void PipelineOrchestrator::UpdateTimerPolicy()
{
    effectiveViewportRate_ = EffectiveViewportRate();
    const int requestedViewportRate = RequestedViewportRate();
    if (requestedViewportRate > effectiveViewportRate_) {
        if (announcedClampedRate_ != effectiveViewportRate_) {
            announcedClampedRate_ = effectiveViewportRate_;
            if (callbacks_.viewportRateClamped) {
                callbacks_.viewportRateClamped(
                    requestedViewportRate, effectiveViewportRate_);
            }
        }
    } else {
        announcedClampedRate_ = 0;
    }
    const bool continuousMotion =
        callbacks_.hasContinuousMotion &&
        callbacks_.hasContinuousMotion();
    const bool mousePan =
        callbacks_.isMousePanActive && callbacks_.isMousePanActive();
    const bool presenterNeedsFrame =
        callbacks_.needsScenePresent && callbacks_.needsScenePresent();
    const bool settling =
        viewportMotionTailTimer_.isValid() &&
        viewportMotionTailTimer_.elapsed() < 150;
    const bool viewportActive =
        continuousMotion || mousePan || viewportDirty_ ||
        presenterNeedsFrame || settling;
    const bool cameraActive =
        callbacks_.cameraActive && callbacks_.cameraActive();
    const double cameraRate =
        callbacks_.cameraFrameRate ? callbacks_.cameraFrameRate() : 0.0;
    const int idleCameraRate =
        cameraRate > 0.0
            ? std::clamp(static_cast<int>(std::lround(cameraRate)),
                         1,
                         effectiveViewportRate_)
            : 30;
    const bool blockingCudaPresent =
        !fenceInteropEnabled_ && callbacks_.usingCuda && callbacks_.usingCuda();
    const int activeViewportRate =
        blockingCudaPresent
            ? std::min(effectiveViewportRate_, idleCameraRate)
            : effectiveViewportRate_;
    const int targetRate =
        viewportActive
            ? activeViewportRate
            : (cameraActive ? idleCameraRate : 10);
    const int intervalMs =
        std::max(1, static_cast<int>(std::lround(1000.0 / targetRate)));
    if (timer_.interval() != intervalMs) {
        timer_.setInterval(intervalMs);
    }
}

void PipelineOrchestrator::OnTick()
{
    QElapsedTimer tickTimer;
    tickTimer.start();
    const qint64 elapsedNanos =
        viewportTickTimer_.isValid()
            ? viewportTickTimer_.nsecsElapsed()
            : 8'000'000;
    viewportTickTimer_.restart();
    const double elapsedSeconds =
        static_cast<double>(elapsedNanos) / 1'000'000'000.0;
    const bool processedCameraFrame =
        callbacks_.tick && callbacks_.tick(elapsedSeconds);
    UpdateTimerPolicy();
    if (processedCameraFrame) {
        RecordFrameTickSample(tickTimer.nsecsElapsed());
    }
}

void PipelineOrchestrator::RecordFrameTickSample(qint64 elapsedNanos)
{
    const float ms = static_cast<float>(elapsedNanos) * 1e-6f;
    frameTickSampleSumMs_ +=
        ms - frameTickSamplesMs_[frameTickSampleIndex_];
    frameTickSamplesMs_[frameTickSampleIndex_] = ms;
    frameTickSampleIndex_ =
        (frameTickSampleIndex_ + 1) % frameTickSamplesMs_.size();
    frameTickSampleCount_ =
        std::min(frameTickSampleCount_ + 1, frameTickSamplesMs_.size());
    frameTickAverageMs_ =
        frameTickSampleSumMs_ / static_cast<float>(frameTickSampleCount_);

    const double cameraRate =
        callbacks_.cameraFrameRate ? callbacks_.cameraFrameRate() : 0.0;
    const float frameBudgetMs =
        cameraRate > 0.0
            ? static_cast<float>(1000.0 / cameraRate)
            : static_cast<float>(1000.0 /
                                 std::max(1, effectiveViewportRate_));
    const TimingPercentiles percentiles = FrameTickPercentiles();
    if (percentiles.p95Ms > frameBudgetMs &&
        frameTickSampleCount_ >= 30u) {
        if (!frameTickOverBudgetTimer_.isValid()) {
            frameTickOverBudgetTimer_.start();
        } else if (!frameTickOverBudgetWarned_ &&
                   frameTickOverBudgetTimer_.elapsed() >= 3000) {
            const bool usingCuda =
                callbacks_.usingCuda && callbacks_.usingCuda();
            const TimingPercentiles capture =
                StagePercentiles(FrameTimingStage::CaptureHandoff);
            const TimingPercentiles preparation =
                StagePercentiles(FrameTimingStage::CpuPreparation);
            const TimingPercentiles submission =
                StagePercentiles(FrameTimingStage::CudaSubmission);
            const TimingPercentiles presentation =
                StagePercentiles(FrameTimingStage::Presentation);
            const TimingPercentiles recording =
                StagePercentiles(FrameTimingStage::RecordingClone);
            qWarning() << "Frame processing p95 exceeded the camera budget:"
                       << percentiles.p95Ms << "ms p95 versus"
                       << frameBudgetMs << "ms over the last"
                       << frameTickSampleCount_ << "frames"
                       << (usingCuda ? "(CUDA path)" : "(passthrough path)")
                       << "| stage p95 ms: capture"
                       << capture.p95Ms
                       << "prepare" << preparation.p95Ms
                       << "CUDA-submit-CPU" << submission.p95Ms
                       << "present" << presentation.p95Ms
                       << "recording-clone" << recording.p95Ms;
            frameTickOverBudgetWarned_ = true;
        }
    } else {
        frameTickOverBudgetTimer_.invalidate();
        frameTickOverBudgetWarned_ = false;
    }
}

} // namespace okuflow

#endif // _WIN32

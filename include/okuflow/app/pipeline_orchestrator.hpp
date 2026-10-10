#pragma once

#if defined(_WIN32) || defined(Q_MOC_RUN)

#include "okuflow/app/settings_store.hpp"

#include <QElapsedTimer>
#include <QTimer>

#include <array>
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace okuflow {

struct TimingPercentiles {
    float p50Ms{-1.0f};
    float p95Ms{-1.0f};
    float p99Ms{-1.0f};
    std::size_t sampleCount{0};

    bool IsValid() const noexcept { return sampleCount > 0; }
};

enum class FrameTimingStage : std::size_t {
    CaptureHandoff = 0,
    CpuPreparation,
    CudaSubmission,
    Presentation,
    RecordingClone,
    Count,
};

// Owns the viewport presentation clock and its instrumentation. Camera
// processing remains a distinct callback: it advances only when the callback
// consumes a fresh frame, while viewport-only motion may present the cached
// scene at the active display rate.
class PipelineOrchestrator final {
public:
    struct Callbacks {
        std::function<bool(double)> tick;
        std::function<bool()> hasContinuousMotion;
        std::function<bool()> isMousePanActive;
        std::function<bool()> needsScenePresent;
        std::function<bool()> cameraActive;
        std::function<double()> cameraFrameRate;
        std::function<bool()> usingCuda;
        std::function<int()> queryDisplayRefreshRate;
        std::function<void(int requestedRate, int effectiveRate)>
            viewportRateClamped;
    };

    PipelineOrchestrator(QObject& context, Callbacks callbacks);
    ~PipelineOrchestrator();

    PipelineOrchestrator(const PipelineOrchestrator&) = delete;
    PipelineOrchestrator& operator=(const PipelineOrchestrator&) = delete;

    void Start();
    void Stop();
    void UpdateTimerPolicy();
    // Thread-safe wakeup used by the Media Foundation callback. Camera
    // processing must be driven by frame arrival rather than by polling a
    // single mailbox at approximately the same rate as the producer.
    void NotifyCameraFrameAvailable(int delayMs = 0);

    void SetViewportRateMode(settings::ViewportRateMode mode);
    settings::ViewportRateMode ViewportRateMode() const;
    void SetViewportFitMode(settings::ViewportFitModeSetting mode);
    settings::ViewportFitModeSetting ViewportFitMode() const;

    void MarkViewportDirty();
    bool IsViewportDirty() const;
    void NotifyViewportMotion();
    void MarkViewportPresented();

    int EffectiveViewportRate() const;
    int DisplayRefreshRate() const;
    float MeasuredViewportRate() const;
    float FrameTickAverageMs() const;
    TimingPercentiles FrameTickPercentiles() const;
    void RecordCaptureToPresentSample(float milliseconds);
    TimingPercentiles CaptureToPresentPercentiles() const;
    void RecordStageSample(FrameTimingStage stage, float milliseconds);
    TimingPercentiles StagePercentiles(FrameTimingStage stage) const;

    bool BeginCameraReconnect(qint64 nowMs);
    void CancelCameraReconnect();
    bool IsCameraReconnectPending() const;
    bool CameraReconnectDue(qint64 nowMs) const;
    bool CameraReconnectExpired(qint64 nowMs) const;
    int CameraReconnectAttempt() const;
    void ScheduleCameraReconnectRetry(qint64 nowMs);

    bool FenceInteropEnabled() const;
    void SetFenceInteropEnabled(bool enabled);
    int RecordCudaFailure();
    void ResetCudaFailures();

private:
    void OnTick();
    void RecordFrameTickSample(qint64 elapsedNanos);
    int RequestedViewportRate() const;

    QTimer timer_;
    std::atomic<bool> running_{false};
    std::atomic<bool> cameraTickQueued_{false};
    Callbacks callbacks_;
    settings::ViewportRateMode viewportRateMode_{
        settings::ViewportRateMode::AutoUpTo120};
    settings::ViewportFitModeSetting viewportFitMode_{
        settings::ViewportFitModeSetting::Fill};
    int displayRefreshHz_{60};
    int effectiveViewportRate_{60};
    int announcedClampedRate_{0};
    float measuredViewportRate_{0.0f};
    int viewportPresentCount_{0};
    bool viewportDirty_{true};
    QElapsedTimer viewportTickTimer_;
    QElapsedTimer viewportMotionTailTimer_;
    QElapsedTimer viewportRateMeasurementTimer_;

    static constexpr std::size_t kTimingSampleCapacity = 240;
    std::array<float, kTimingSampleCapacity> frameTickSamplesMs_{};
    std::size_t frameTickSampleIndex_{0};
    std::size_t frameTickSampleCount_{0};
    float frameTickSampleSumMs_{0.0f};
    float frameTickAverageMs_{-1.0f};
    std::array<float, kTimingSampleCapacity> captureToPresentSamplesMs_{};
    std::size_t captureToPresentSampleIndex_{0};
    std::size_t captureToPresentSampleCount_{0};
    static constexpr std::size_t kFrameTimingStageCount =
        static_cast<std::size_t>(FrameTimingStage::Count);
    std::array<std::array<float, kTimingSampleCapacity>,
               kFrameTimingStageCount>
        stageSamplesMs_{};
    std::array<std::size_t, kFrameTimingStageCount> stageSampleIndexes_{};
    std::array<std::size_t, kFrameTimingStageCount> stageSampleCounts_{};
    QElapsedTimer frameTickOverBudgetTimer_;
    bool frameTickOverBudgetWarned_{false};

    bool cameraReconnectPending_{false};
    int cameraReconnectAttempt_{0};
    qint64 cameraReconnectStartedMs_{0};
    qint64 cameraReconnectNextAttemptMs_{0};

    bool fenceInteropEnabled_{false};
    int consecutiveCudaFailures_{0};
};

} // namespace okuflow

#endif // _WIN32

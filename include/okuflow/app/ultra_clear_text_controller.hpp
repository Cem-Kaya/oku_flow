#pragma once

#if defined(_WIN32) || defined(Q_MOC_RUN)

#include "okuflow/common/ultra_clear_text.hpp"
#include "okuflow/ui/ultra_clear_text_overlay.hpp"

#include <QObject>
#include <QPointer>
#include <QRectF>

QT_BEGIN_NAMESPACE
class QTimer;
QT_END_NAMESPACE

#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>

namespace okuflow {

class UltraClearTextPanel;

// Everything that defines the processed-scene geometry. Any change starts a
// new geometry generation and immediately revokes recognized text.
struct UctGeometryKey {
    std::uint64_t sourceEpoch{};
    std::uint32_t sceneWidth{};
    std::uint32_t sceneHeight{};
    int rotationQuarterTurns{};
    bool keystoneEnabled{};
    bool stabilizationEnabled{};

    bool operator==(const UctGeometryKey&) const = default;
};

// Per-presentation inputs, sampled on the Qt UI thread at present time.
struct UctFrameContext {
    UctGeometryKey geometry;
    std::uint64_t frameSeq{};
    ViewTransform transform;     // full-scene -> viewport (not the SR ROI remap)
    std::uint32_t viewportWidth{};
    std::uint32_t viewportHeight{};
    bool cameraActive{};
    // Temporal Smooth / Extra Stable present multi-frame composites whose
    // provenance UCT cannot state truthfully; UCT pauses instead.
    bool compositeFrames{};
    int strokeWeight{};
};

struct UctTiming {
    std::int64_t probeIntervalMs{80};
    std::int64_t readbackTimeoutMs{1000};
    std::int64_t refreshIntervalMs{1500};
    double stableSimilarity{0.92};
    int probeDecimation{2};
};

// Owns Ultra Clear Text runtime state on the UI thread. The presentation path
// calls OnPresented() every frame; it only compares small values, reprojects
// cached words and decides whether one more asynchronous viewport copy is
// wanted. Recognition runs on the scheduler worker and is admitted later
// through stamp, age, residual and temporal-agreement checks.
class UltraClearTextController final : public QObject {
    Q_OBJECT
public:
    using RecognizerFactory = std::function<std::unique_ptr<uct::Recognizer>()>;
    using Clock = std::function<std::int64_t()>;

    UltraClearTextController(UltraClearTextOverlay* overlay,
                             UltraClearTextPanel* panel,
                             RecognizerFactory factory,
                             QObject* parent = nullptr,
                             Clock clock = {},
                             UctTiming timing = {});
    ~UltraClearTextController() override;

    void SetEnabled(bool enabled);
    bool IsEnabled() const noexcept { return enabled_; }
    void SetShowOriginal(bool showOriginal);

    // Presentation clock. Never blocks; returns true when a viewport copy
    // should be requested with this presentation.
    bool OnPresented(const UctFrameContext& context);
    // requestId 0 means the presenter's readback ring was full.
    void OnReadbackRequested(std::uint64_t requestId, const UctFrameContext& context);
    bool OwnsReadback(std::uint64_t requestId) const noexcept;
    void OnReadbackCompleted(std::uint64_t requestId, const std::uint8_t* bgra, int width,
                             int height, int strideBytes);
    // CPU presentation path: pixels are already in system memory.
    void OnCpuViewport(const UctFrameContext& context, const std::uint8_t* bgra, int width,
                       int height, int strideBytes);

    // Diagnostics and fixtures.
    const uct::OverlayModel& Model() const noexcept { return model_; }
    uct::SchedulerStats SchedulerStatistics() const;
    uct::BackendStatus BackendStatusSnapshot() const { return backend_; }
    std::uint64_t GeometryGeneration() const noexcept { return geometryGeneration_; }
    std::uint64_t PresentCount() const noexcept { return presentCount_; }
    std::uint64_t RejectedResults() const noexcept { return rejectedResults_; }
    std::uint64_t AcceptedResults() const noexcept { return acceptedResults_; }
    bool HasPendingReadback() const noexcept { return pendingReadbackId_ != 0; }
    QString StatusSource() const { return statusSource_; }

signals:
    void recognitionAdmitted(int verdict);

private:
    void EnsureScheduler();
    void Invalidate(const QString& statusSource);
    void HandleProbe(const uct::SourceStamp& stamp, const std::uint8_t* bgra, int width,
                     int height, int strideBytes);
    void HandleResult(uct::RecognitionResult result);
    void HandleBackendStatus(uct::BackendStatus status);
    void RefreshOverlay(std::int64_t nowMs);
    void UpdateReadingText();
    void UpdateStatus();
    void SetStatus(const QString& source, bool important = false);
    uct::SourceStamp MakeStamp(const UctFrameContext& context, std::int64_t nowMs) const;
    uct::SourceStamp CurrentScene() const;

    QPointer<UltraClearTextOverlay> overlay_;
    QPointer<UltraClearTextPanel> panel_;
    RecognizerFactory factory_;
    Clock clock_;
    UctTiming timing_;
    std::unique_ptr<uct::RecognitionScheduler> scheduler_;
    uct::OverlayModel model_;
    uct::BackendStatus backend_;

    bool enabled_{};
    bool showOriginal_{};
    bool compositePaused_{};
    bool cameraActive_{};
    bool haveGeometry_{};
    UctGeometryKey geometry_;
    std::uint64_t geometryGeneration_{};
    std::uint64_t currentFrameSeq_{};

    std::uint64_t pendingReadbackId_{};
    uct::SourceStamp pendingStamp_;
    std::int64_t pendingSinceMs_{};
    std::int64_t lastProbeRequestMs_{std::numeric_limits<std::int64_t>::min() / 2};

    std::optional<uct::ProbeFrame> latestProbe_;
    int stableProbes_{};
    bool changedSinceSubmit_{true};
    std::int64_t lastSubmitMs_{std::numeric_limits<std::int64_t>::min() / 2};
    QRectF lastJobCoverage_;
    bool lastResultMoved_{};
    bool lastResultEmpty_{};

    int consecutiveFailures_{};
    QTimer* watchdog_{};
    std::int64_t lastPresentMs_{};
    QVector<UltraClearTextOverlay::SceneWord> overlayWords_;

    std::uint64_t presentCount_{};
    std::uint64_t rejectedResults_{};
    std::uint64_t acceptedResults_{};
    int shownWords_{-1};
    QString statusSource_;
};

} // namespace okuflow

#endif // defined(_WIN32) || defined(Q_MOC_RUN)

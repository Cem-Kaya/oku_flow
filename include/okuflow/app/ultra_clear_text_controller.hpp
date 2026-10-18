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
#include <vector>

namespace okuflow {

class UltraClearTextPanel;

// Processing settings that decide where text lands in the processed scene
// or how it looks. Used twice: as *applied* (what a cached scene was really
// built with) and as *requested* (what the user asks for now, mapped to what
// the active pipeline path would apply).
struct UctSceneSettings {
    std::uint64_t sourceEpoch{};
    int rotationQuarterTurns{};
    bool keystone{};        // CUDA keystone warp applied
    bool stabilization{};   // CUDA stabilization warp applied
    // Discrete warp resets/steps (keystone step, stabilization/keystone
    // reset). Continuous keystone/stabilization tracking between those is
    // not exposed by the CUDA surface; residual validation covers it.
    std::uint64_t warpRevision{};
    std::uint64_t colorScheme{}; // exact display LUT generation, 0 for normal
    bool operator==(const UctSceneSettings&) const = default;
};

// Everything that defines the processed-scene geometry. Any change starts a
// new geometry generation and immediately revokes recognized text.
struct UctGeometryKey {
    UctSceneSettings settings;
    std::uint32_t sceneWidth{};
    std::uint32_t sceneHeight{};
    // Separate content authority for base/spatial versus neural pixels from
    // the same camera frame; changing it revokes in-flight OCR/readbacks.
    bool neuralEnhancement{};

    bool operator==(const UctGeometryKey&) const = default;
};

// Per-presentation inputs, sampled on the Qt UI thread at present time.
struct UctFrameContext {
    UctGeometryKey geometry;      // applied: baked into the presented scene
    UctSceneSettings requested;   // requested now; a mismatch revokes text
    std::uint64_t frameSeq{};     // camera frame baked into the presented scene
    ViewTransform transform;      // full-scene -> viewport (not the SR ROI remap)
    std::uint32_t viewportWidth{};
    std::uint32_t viewportHeight{};
    bool cameraActive{};
    // Temporal Smooth / Extra Stable present multi-frame composites whose
    // provenance UCT cannot state truthfully; UCT pauses instead. True when
    // either the cached scene or the request is a composite.
    bool compositeFrames{};
    int strokeWeight{};
};

// Snapshot of what one cached processed scene was built with. Taken only
// when processing succeeded, so deferred, busy or failed processing and
// re-presents of cached pixels keep the old scene's provenance.
struct UctSceneSnapshot {
    bool valid{};
    UctGeometryKey geometry;
    std::uint64_t frameSeq{};
    bool composite{};
};

// The production seam between OkuFlowApp's processing paths and Ultra Clear
// Text. One snapshot per cached scene (CUDA shared texture, CPU scene
// buffer); presentation builds the context from the snapshot of the scene it
// actually presents plus the current request.
class UctSceneProvenance {
public:
    // CUDA processing was admitted and queued for `frameSeq`. `applied`
    // carries the settings actually submitted with this frame.
    void OnCudaSceneProcessed(const UctSceneSettings& applied, std::uint32_t width,
                              std::uint32_t height, std::uint64_t frameSeq, bool composite)
    {
        cuda_ = {true, {Normalize(applied), width, height}, frameSeq, composite};
    }
    // CPU fallback: rotation is applied on the CPU; keystone,
    // stabilization and display colors are not.
    void OnCpuSceneProcessed(std::uint64_t sourceEpoch, int rotationQuarterTurns,
                             std::uint32_t width, std::uint32_t height, std::uint64_t frameSeq,
                             bool composite)
    {
        UctSceneSettings applied;
        applied.sourceEpoch = sourceEpoch;
        applied.rotationQuarterTurns = NormalizeTurns(rotationQuarterTurns);
        cpu_ = {true, {applied, width, height}, frameSeq, composite};
    }
    void InvalidateCuda() { cuda_ = {}; }
    void Reset()
    {
        cuda_ = {};
        cpu_ = {};
    }

    const UctSceneSnapshot& Cuda() const noexcept { return cuda_; }
    const UctSceneSnapshot& Cpu() const noexcept { return cpu_; }

    // `requested` is the full user request; the CPU path maps it to what
    // that path can apply so a lasting CPU fallback is not "settling".
    static UctSceneSettings RequestFor(bool cudaPath, UctSceneSettings requested)
    {
        if (!cudaPath) {
            requested.keystone = false;
            requested.stabilization = false;
            requested.colorScheme = 0;
        }
        return Normalize(requested);
    }

    // Warp revisions only matter while a warp is applied; rotation is mod 4.
    static UctSceneSettings Normalize(UctSceneSettings settings)
    {
        settings.rotationQuarterTurns = NormalizeTurns(settings.rotationQuarterTurns);
        if (!settings.keystone && !settings.stabilization) {
            settings.warpRevision = 0;
        }
        return settings;
    }

    UctFrameContext Context(bool cudaPath, const UctSceneSettings& requested, bool requestedComposite,
                            const ViewTransform& transform, std::uint32_t viewportWidth,
                            std::uint32_t viewportHeight, bool cameraActive, int strokeWeight) const
    {
        const UctSceneSnapshot& scene = cudaPath ? cuda_ : cpu_;
        UctFrameContext context;
        context.geometry = scene.geometry;
        context.requested = RequestFor(cudaPath, requested);
        context.frameSeq = scene.frameSeq;
        context.transform = transform;
        context.viewportWidth = viewportWidth;
        context.viewportHeight = viewportHeight;
        // Without a snapshot there is no truthful provenance: behave as if
        // no camera image were shown.
        context.cameraActive = cameraActive && scene.valid;
        context.compositeFrames = scene.composite || requestedComposite;
        context.strokeWeight = strokeWeight;
        return context;
    }

    static int NormalizeTurns(int turns) { return ((turns % 4) + 4) % 4; }

private:
    UctSceneSnapshot cuda_;
    UctSceneSnapshot cpu_;
};

struct UctTiming {
    std::int64_t probeIntervalMs{80};
    double probeCheckBudgetMs{60.0}; // off-thread residual budget per probe
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
    // Receipt from the actual presenter, never from submission/preflight.
    void OnPresentationSucceeded();
    void OnSourceRetired(); // immediate revocation; does not wait for another presentation
    // The presenter rejected the frame: the screen still shows older pixels,
    // so words reprojected for this context are hidden until the next
    // successful presentation.
    void OnPresentationFailed();
    // requestId 0 means the presenter's readback ring was full.
    void OnReadbackRequested(std::uint64_t requestId, const UctFrameContext& context);
    bool OwnsReadback(std::uint64_t requestId) const noexcept;
    // Takes ownership of the presenter's readback vector by swapping it with
    // a recycled one (no copy). Call after every other consumer of the same
    // readback has read the pixels.
    void OnReadbackCompleted(std::uint64_t requestId, std::vector<std::uint8_t>& bgra, int width,
                             int height, int strideBytes);
    // Copying variant for caller-owned memory (fixtures).
    void OnReadbackCompleted(std::uint64_t requestId, const std::uint8_t* bgra, int width,
                             int height, int strideBytes);
    // CPU presentation path: the presentation buffer is reused by the app,
    // so its pixels are copied once into a pooled buffer.
    void OnCpuViewport(const UctFrameContext& context, const std::uint8_t* bgra, int width,
                       int height, int strideBytes);
    void OnDeviceScene(const UctFrameContext& context, uct::SharedDeviceImage device);
    // A pool/copy failure is recoverable input health, not a failed OCR model.
    void OnAnalysisHealth(bool failed);

    // Diagnostics and fixtures.
    const uct::OverlayModel& Model() const noexcept { return model_; }
    uct::SchedulerStats SchedulerStatistics() const;
    uct::ProbeWorkerStats ProbeStatistics() const;
    std::uint64_t PixelAllocations() const { return pool_->Allocations(); }
    bool IsSettling() const noexcept { return settling_; }
    // Wall time of the latest UI-thread probe-outcome handling (diagnostics).
    double LastOutcomeHandlingMs() const noexcept { return lastOutcomeMs_; }
    double LastResultHandlingMs() const noexcept { return lastResultMs_; }
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
    void SubmitProbe(const uct::SourceStamp& stamp, uct::SharedPixels pixels);
    void HandleProbeOutcome(uct::ProbeOutcome outcome);
    void HandleResult(uct::RecognitionResult result);
    void HandleBackendStatus(uct::BackendStatus status);
    void RefreshOverlay(std::int64_t nowMs);
    void UpdateReadingText();
    bool AnalysisHealthy() const;
    bool PresentationAvailable(std::int64_t nowMs) const;
    void UpdateStatus();
    void SetStatus(const QString& source, bool important = false);
    uct::SourceStamp MakeStamp(const UctFrameContext& context, std::int64_t nowMs) const;
    uct::SourceStamp CurrentScene() const;

    QPointer<UltraClearTextOverlay> overlay_;
    QPointer<UltraClearTextPanel> panel_;
    RecognizerFactory factory_;
    Clock clock_;
    UctTiming timing_;
    std::shared_ptr<uct::PixelPool> pool_;
    std::unique_ptr<uct::RecognitionScheduler> scheduler_;
    std::unique_ptr<uct::RecognitionScheduler> deviceProbeReader_;
    uct::SharedDeviceImage deviceCandidate_;
    std::int64_t deviceCandidateSinceMs_{};
    std::uint64_t schedulerGeneration_{}; // fences callbacks across explicit initialization retry
    std::unique_ptr<uct::ProbeWorker> probeWorker_;
    uct::OverlayModel model_;
    uct::BackendStatus backend_;

    bool enabled_{};
    bool showOriginal_{};
    bool compositePaused_{};
    bool cameraActive_{};
    bool haveGeometry_{};
    bool settling_{};
    bool presentationFailed_{};
    bool presentationAvailable_{};
    UctGeometryKey geometry_;
    std::uint64_t geometryGeneration_{};
    std::uint64_t currentFrameSeq_{};

    std::uint64_t pendingReadbackId_{};
    uct::SourceStamp pendingStamp_;
    std::int64_t pendingSinceMs_{};
    std::int64_t lastProbeRequestMs_{std::numeric_limits<std::int64_t>::min() / 2};

    std::shared_ptr<const uct::ProbeFrame> latestProbe_;
    int stableProbes_{};
    bool changedSinceSubmit_{true};
    std::int64_t lastSubmitMs_{std::numeric_limits<std::int64_t>::min() / 2};
    QRectF lastJobCoverage_;
    bool lastResultMoved_{};
    bool lastResultEmpty_{};

    int consecutiveFailures_{};
    int consecutiveInputFailures_{};
    int consecutiveProbeFailures_{};
    bool analysisPoolFailed_{};
    QTimer* watchdog_{};
    std::int64_t lastPresentMs_{};
    QVector<UltraClearTextOverlay::SceneWord> overlayWords_;

    std::uint64_t presentCount_{};
    std::uint64_t rejectedResults_{};
    std::uint64_t acceptedResults_{};
    int shownWords_{-1};
    QString statusSource_;
    QString readingText_;
    double lastOutcomeMs_{};
    double lastResultMs_{};
};

} // namespace okuflow

#endif // defined(_WIN32) || defined(Q_MOC_RUN)

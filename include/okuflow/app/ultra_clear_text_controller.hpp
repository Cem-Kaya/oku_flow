#pragma once

#if defined(_WIN32) || defined(Q_MOC_RUN)

#include "okuflow/common/ultra_clear_text.hpp"
#include "okuflow/ui/uct_screen_text_accessible.hpp"
#include "okuflow/ui/ultra_clear_text_overlay.hpp"

#include <QObject>
#include <QPointer>
#include <QSize>
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
    // Display truth for annotations/observations: Temporal Smooth and Extra
    // Stable both combine/hold frames. True for a cached or requested composite.
    // UCT's separate analysis eligibility is below.
    bool compositeFrames{};
    // UCT-specific: the analysis branch cannot truthfully describe this scene
    // (CPU debug composites). Display Temporal Smooth and CUDA Extra Stable
    // are compositeFrames but supported: the former through a neutral paired
    // analysis image, the latter through per-capture hold provenance.
    bool unsupportedCompositeFrames{};
    // CUDA Extra Stable may show a held earlier acquisition or a recovery
    // crossfade. Device analysis must then carry the frame's device hold
    // record, resolved on a worker; an image without one is never live.
    bool heldFrames{};
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
    bool unsupportedComposite{};
    bool heldFrames{};
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
                              std::uint32_t height, std::uint64_t frameSeq, bool composite,
                              std::optional<bool> unsupportedComposite = std::nullopt,
                              bool heldFrames = false)
    {
        cuda_ = {true, {Normalize(applied), width, height}, frameSeq, composite,
                 unsupportedComposite.value_or(composite), heldFrames};
    }
    // CPU fallback: rotation is applied on the CPU; keystone,
    // stabilization and display colors are not.
    void OnCpuSceneProcessed(std::uint64_t sourceEpoch, int rotationQuarterTurns,
                             std::uint32_t width, std::uint32_t height, std::uint64_t frameSeq,
                             bool composite,
                             std::optional<bool> unsupportedComposite = std::nullopt)
    {
        UctSceneSettings applied;
        applied.sourceEpoch = sourceEpoch;
        applied.rotationQuarterTurns = NormalizeTurns(rotationQuarterTurns);
        cpu_ = {true, {applied, width, height}, frameSeq, composite,
                unsupportedComposite.value_or(composite)};
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
                            std::uint32_t viewportHeight, bool cameraActive, int strokeWeight,
                            std::optional<bool> requestedUnsupportedComposite = std::nullopt,
                            bool requestedHeldFrames = false) const
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
        context.unsupportedCompositeFrames =
            scene.unsupportedComposite || requestedUnsupportedComposite.value_or(requestedComposite);
        // Either the cached scene or the request may hold frames: require the
        // record whenever either does, so a toggle never admits unnamed pixels.
        context.heldFrames = cudaPath && (scene.heldFrames || requestedHeldFrames);
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
    // Current-job deadline, independent of an immutable frame's media age.
    std::int64_t immutableJobTimeoutMs{30000};
    std::int64_t mediaRecognitionIntervalMs{350}; // paced advancing-media OCR, not a per-frame task
    // Wall-clock bound for advancing-media validation freshness. The media
    // bound lives in the model's freshness policy (350 ms of media PTS);
    // a no-analysis gap is retained only while both bounds hold.
    std::int64_t mediaValidationFreshnessMs{350};
    // Paused visible detail: a view must stay unchanged this long before one
    // bounded region of the retained selected/neighbour pair is re-detected.
    std::int64_t immutableDetailSettleMs{300};
    // Detector enlargement bound for that region (clamped to [1, 6]).
    double immutableDetailMagnification{uct::kMaximumDetailMagnification};
};

// Full processed-scene input retained by the selected-media owner. The
// timestamp belongs to the source image; it is never refreshed to admit OCR.
// `device` is an optional independently owned full-scene, pre-zoom device
// image of the same frame. When it matches the scene (source epoch, frame
// sequence and dimensions) recognition prefers the scheduler's device path
// and any CPU readback happens on a worker thread only; otherwise valid
// `pixels` are the CPU fallback. `frameSeq`/`timestampMs` stay the
// authoritative frame identity and real media PTS, and the device pointer
// identity participates in same-frame/discontinuity checks. The trailing
// field keeps existing three-field aggregate initializers valid.
struct UctImmutableFrame {
    uct::SharedPixels pixels;
    std::uint64_t frameSeq{};
    std::int64_t timestampMs{};
    uct::SharedDeviceImage device;
};

// Explicit media authority, separate from live camera/presentation liveness.
// Changing sourceAuthority (e.g. seek generation), geometry or selected frame
// revokes earlier work. Pixels or the device image must cover the complete
// processed scene.
struct UctImmutableScene {
    UctGeometryKey geometry;
    std::uint64_t sourceAuthority{};
    UctImmutableFrame selected;
    std::optional<UctImmutableFrame> corroboration;
    ViewTransform transform;
    std::uint32_t viewportWidth{};
    std::uint32_t viewportHeight{};
    int strokeWeight{};
};

// A receipt for a successfully presented advancing media frame. timestampMs
// is its real media PTS (milliseconds); UI/job clocks remain separate.
struct UctAdvancingMediaScene {
    UctGeometryKey geometry;
    std::uint64_t sourceAuthority{};
    UctImmutableFrame current;
    ViewTransform transform;
    std::uint32_t viewportWidth{};
    std::uint32_t viewportHeight{};
    int strokeWeight{};
};

// A successful advancing-media presentation receipt WITHOUT any analysis
// pixels, for presented frames whose display has no analysis lease (pool
// pressure, probe/OCR pacing, or a copy the presenter withheld). It carries
// only what the presenter can state truthfully about the displayed frame:
// its geometry, source authority, frame sequence, real media PTS and view.
// It never restamps an older analysis image as the displayed frame, never
// submits a probe or OCR job, and never renews validation. The controller
// advances its display/model clock from frameSeq/timestampMs and keeps the
// previously validated overlay only while that real validation stays fresh
// in both media time and wall time.
struct UctAdvancingMediaPresentation {
    UctGeometryKey geometry;
    std::uint64_t sourceAuthority{};
    std::uint64_t frameSeq{};     // frame actually presented
    std::int64_t timestampMs{};   // its real media PTS, never an analysis PTS
    ViewTransform transform;
    std::uint32_t viewportWidth{};
    std::uint32_t viewportHeight{};
    int strokeWeight{};
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
    bool IsEnabledByUser() const noexcept { return enabledByUser_; }
    void SetTextClarityEnabled(bool enabled);
    bool IsEnabled() const noexcept { return enabled_; }
    // Retire a previous worker asynchronously and fence its queued callbacks.
    void ReplaceRecognizerFactory(RecognizerFactory factory);
    void SetShowOriginal(bool showOriginal);

    // Selected media uses the same recognition scheduler as live input.
    // A successful selection recognizes its owned complete scene once, then
    // optionally one real distinct neighbour. Invalid input still suspends
    // hidden live analysis until ClearImmutableScene() is called. A first
    // usable neighbour arriving for the exact retained selected lease/frame/
    // PTS/geometry/authority attaches without restarting selected analysis;
    // later optional neighbour changes are not new selected acquisitions.
    bool SetImmutableScene(UctImmutableScene scene);
    // A processed media view may not have provable single-frame text
    // correspondence (neural output). Preserve the user's UCT preference, but
    // revoke its words, jobs and UIA exposure. Extra Stable media is supported
    // through the per-capture hold record and is not suppressed for that
    // reason; `extraStable` only attributes this suppression to shake-held
    // frames in the status line.
    void SetMediaOverlaySuppressed(bool suppressed, bool extraStable = false);
    // Successful native media receipt only. Retains one latest scene lease,
    // paces full-scene probes/OCR, and never fabricates a camera presentation.
    bool OnAdvancingMediaPresented(UctAdvancingMediaScene scene);
    // Successful advancing-media presentation with no analysis lease for the
    // displayed frame. Advances only the display/model clock and view;
    // submits no probe/OCR and renews no validation, recognition stamp or
    // analysis health. A geometry/source-authority change, seek, backward
    // frame/PTS, or a same-sequence frame with a different PTS revokes all
    // text immediately and records the receipt as a display-only chain
    // awaiting a real analysis frame. Returns true only when the receipt
    // continues the current chain (forward, or the same frame reprojected);
    // a discontinuity, the first establishing receipt and an invalid view
    // return false. Presenter faults still use OnMediaPresentationFailed().
    bool OnAdvancingMediaPresentedWithoutAnalysis(UctAdvancingMediaPresentation receipt);
    void UpdateImmutableView(const ViewTransform& transform, std::uint32_t viewportWidth,
                             std::uint32_t viewportHeight, int strokeWeight = 0);
    // Media presenter faults hide every text surface until a real successful
    // receipt. View-only updates and camera notifications cannot reopen it.
    // SetImmutableScene/OnAdvancingMediaPresented already carry that receipt;
    // use this success signal after a committed view-only media presentation.
    void OnMediaPresentationSucceeded();
    void OnMediaPresentationFailed();
    void ClearImmutableScene();
    // Selected-media ownership, including advancement, suspends hidden live OCR.
    // A display-only advancing chain (no analysis frame yet) also suspends it.
    bool HasImmutableScene() const noexcept
    {
        return immutableScene_.has_value() || advancingMediaScene_.has_value() ||
               advancingDisplay_.has_value();
    }
    bool HasAdvancingMediaScene() const noexcept
    {
        return advancingMediaScene_.has_value() || advancingDisplay_.has_value();
    }
    // Latest advancing display receipt, separate from the last analysis input.
    bool HasAdvancingDisplayReceipt() const noexcept { return advancingDisplay_.has_value(); }
    std::uint64_t AdvancingDisplayFrameSeq() const noexcept
    {
        return advancingDisplay_ ? advancingDisplay_->frameSeq : std::uint64_t{0};
    }
    std::int64_t AdvancingDisplayTimestampMs() const noexcept
    {
        return advancingDisplay_ ? advancingDisplay_->timestampMs : std::int64_t{0};
    }

    // Presentation clock. Never blocks; returns true when a viewport copy
    // should be requested with this presentation.
    bool OnPresented(const UctFrameContext& context);
    // Receipt from the actual presenter, never from submission/preflight.
    void OnPresentationSucceeded();
    // A busy frame slot submitted nothing. Keep the last successful projection
    // without renewing its watchdog; an actual presenter fault still hides it.
    void OnPresentationDeferred();
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
    // An Extra Stable capture names its acquisition only after a worker reads
    // its device record. Crossfade probes may hide text but never validate,
    // admit or start OCR; a held image is read once per held acquisition.
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
    // UI Automation exposure of the drawn text (null without a render target).
    UctScreenTextAccessibility* ScreenTextAccessibility() const noexcept { return screenText_; }
    // Lines the overlay currently replaces, as exposed to screen readers:
    // empty unless UCT is drawing glyphs now, each glyph word cross-checked
    // against the overlay's word set.
    UctScreenTextSnapshot ScreenTextSnapshot() const;
    // Visible-detail diagnostics: completed/abandoned regions for the current
    // selection, and the pair currently in flight (if any).
    std::uint64_t ImmutableDetailPasses() const noexcept { return immutableDetailPasses_; }
    std::optional<uct::DetailFocus> ActiveImmutableDetail() const { return immutableDetailFocus_; }

signals:
    void recognitionAdmitted(int verdict);

private:
    void EnsureScheduler();
    void Invalidate(const QString& statusSource);
    void SubmitProbe(const uct::SourceStamp& stamp, uct::SharedPixels pixels);
    void HandleProbeOutcome(uct::ProbeOutcome outcome);
    void HandleResult(uct::RecognitionResult result);
    void HandleBackendStatus(uct::BackendStatus status);
    void ApplyEnabled(bool enabled);
    void SubmitImmutableIfNeeded();
    void ScheduleImmutableRetry(bool selected, bool preparing = false);
    // Exact selected-frame proof survives a failed optional refinement. This
    // is presentation authority only, never permission to submit/admit work
    // through an unhealthy input/backend. Source changes still Invalidate.
    bool HasProvenImmutableArtifact() const;
    void StopImmutableRefinement();
    void HandleImmutableResult(uct::RecognitionResult result);
    // Visible detail of a paused selection: after the view settles, re-detect
    // only the visible source region of the SAME selected image and its real
    // neighbour at a bounded enlargement. Both stay their original
    // acquisitions; new glyphs still need the neighbour's independent proof.
    void ScheduleImmutableDetail();
    void SubmitImmutableDetailIfUseful();
    std::optional<uct::DetailFocus> ImmutableDetailFocus() const;
    void FinishImmutableDetail();
    uct::SourceStamp ImmutableStamp(const UctImmutableFrame& frame) const;
    void SubmitAdvancingMediaProbe();
    // Media model clock: the latest DISPLAYED frame's real media PTS while
    // advancing (with or without an analysis lease), never the PTS of the
    // last analysis input restamped onto a newer displayed frame.
    std::int64_t ModelTime(std::int64_t wallNowMs) const;
    // True unless advancing text's last REAL validation is wall-stale for the
    // currently displayed frame. The exact validated frame still displayed
    // is exempt, mirroring the model's media-time freshness rule. Receipts
    // without pixels never refresh this.
    bool AdvancingValidationFresh(std::int64_t wallNowMs) const;
    void NoteAdvancingValidation(std::int64_t wallNowMs);
    void RefreshOverlay(std::int64_t nowMs);
    void UpdateReadingText(bool shareTimingRevision = false);
    void NoteTimingAdmission(const uct::RecognitionResult& result, uct::AdmitVerdict verdict,
                             std::uint32_t flags = 0);
    void UpdatePositionedTimingPublication(bool changed, bool positioned, bool clear = false);
    void UpdatePaneTimingPublication(bool sharePositionedRevision, bool clear);
    bool AnalysisHealthy() const;
    bool PresentationAvailable(std::int64_t nowMs) const;
    void UpdateStatus();
    void SetStatus(const QString& source, bool important = false);
    uct::SourceStamp MakeStamp(const UctFrameContext& context, std::int64_t nowMs) const;
    uct::SourceStamp CurrentScene() const;

    QPointer<UltraClearTextOverlay> overlay_;
    QPointer<UltraClearTextPanel> panel_;
    QPointer<UctScreenTextAccessibility> screenText_;
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
    // Diagnostic-only: never derive a publication ticket from the current
    // displayed frame or a timer. Promote a model-admission key only when a
    // surface snapshot actually changes; pane-only changes leave positioned
    // provenance/revision intact, and projection keeps both tickets.
    std::optional<uct::timing::Key> timingAdmission_;
    std::uint32_t timingAdmissionFlags_{};
    uct::timing::PublicationTicket timingPositionedPublication_, timingPanePublication_;
    std::uint64_t timingPublicationRevision_{};

    std::optional<UctImmutableScene> immutableScene_;
    std::optional<UctAdvancingMediaScene> advancingMediaScene_;
    // Latest successful advancing DISPLAY receipt (analysis-carrying or
    // without-analysis). This is the display/model clock and the continuity
    // basis; advancingMediaScene_ above remains the last real analysis
    // input and is never restamped with this receipt's frame/PTS.
    std::optional<UctAdvancingMediaPresentation> advancingDisplay_;
    // Wall time and frame of the last real probe/recognition validation of
    // advancing text. Only HandleProbeOutcome/HandleResult refresh it.
    std::int64_t advancingValidatedWallMs_{};
    std::uint64_t advancingValidatedFrameSeq_{};
    bool haveAdvancingValidation_{};
    // Identity (not ownership) of the advancing scene's device image. The
    // strong lease is released once the probe reader consumes it, so the
    // two-slot analysis pool is not pinned by receipt metadata; the weak
    // identity still detects a different image reused for the same frame.
    std::weak_ptr<const uct::DeviceImage> advancingDeviceIdentity_;
    // Receipt-time input validity, retained after the device lease release
    // so presentation availability keeps tracking the displayed scene.
    bool advancingReceiptValid_{};
    std::uint64_t advancingMediaJob_{};
    std::int64_t advancingMediaSubmittedAtMs_{};
    std::int64_t advancingMediaLastSubmitMs_{std::numeric_limits<std::int64_t>::min() / 2};
    std::uint64_t advancingMediaProbeFrame_{};
    bool advancingMediaHaveProbe_{};
    std::optional<uct::RecognitionResult> immutableSelectedResult_;
    std::uint64_t immutableSelectedJob_{};
    std::uint64_t immutableCorroborationJob_{};
    std::int64_t immutableSubmittedAtMs_{};
    bool immutableSelectedAttempted_{};
    unsigned immutableRetryCount_{};
    bool immutableRetryScheduled_{};
    bool immutableRetryExhausted_{};
    std::optional<std::int64_t> immutableRetryStartedAtMs_;
    bool immutableCorroborationAttempted_{};
    QTimer* immutableDetailTimer_{};
    std::optional<uct::DetailFocus> immutableDetailFocus_; // pair in flight
    struct DetailCoverage { QRect region; double magnification{}; };
    QVector<DetailCoverage> immutableDetailDone_; // bounded, current selection only
    std::uint64_t immutableDetailPasses_{};

    bool enabled_{};
    bool enabledByUser_{};
    bool textClarityEnabled_{true};
    bool showOriginal_{};
    bool compositePaused_{};
    // Which pause reason the status line names: a stabilized (Extra Stable)
    // scene or media suppression attributed to shake-held frames, versus the
    // CPU debug composite. Updated from each presented context.
    bool compositeExtraStable_{};
    bool mediaOverlaySuppressed_{};
    bool mediaSuppressionExtraStable_{};
    bool cameraActive_{};
    bool haveGeometry_{};
    bool settling_{};
    bool presentationFailed_{};
    bool presentationAvailable_{};
    UctGeometryKey geometry_;
    std::uint64_t geometryGeneration_{};
    std::uint64_t currentFrameSeq_{};
    std::uint64_t attemptedFrameSeq_{};

    std::uint64_t pendingReadbackId_{};
    uct::SourceStamp pendingStamp_;
    std::int64_t pendingSinceMs_{};
    std::int64_t lastProbeRequestMs_{std::numeric_limits<std::int64_t>::min() / 2};

    std::shared_ptr<const uct::ProbeFrame> latestProbe_;
    int stableProbes_{};
    bool changedSinceSubmit_{true};
    // A real residual failure needs replacement OCR even when other accepted
    // lines still cover the view. Persist across the existing pacing delay;
    // an unchecked/out-of-view/composite probe does not create this demand.
    bool probeInvalidatedSinceSubmit_{};
    std::int64_t lastSubmitMs_{std::numeric_limits<std::int64_t>::min() / 2};
    QRectF lastJobCoverage_;
    // Acquisition of the last submitted live/advancing OCR job. A held image
    // of the same acquisition cannot add an observation, so it is not re-read
    // while text derived from it remains.
    std::uint64_t lastOcrAcquisition_{};
    std::uint64_t lastOcrGeneration_{};
    std::uint64_t lastOcrJob_{}; // a rejected older job cannot reset a newer marker
    bool haveLastOcr_{};
    bool lastResultMoved_{};
    bool lastResultEmpty_{};

    int consecutiveFailures_{};
    int consecutiveInputFailures_{};
    int consecutiveProbeFailures_{};
    bool analysisPoolFailed_{};
    QTimer* watchdog_{};
    std::int64_t lastPresentMs_{};
    ViewTransform attemptedTransform_;
    ViewTransform presentedTransform_;
    QSize attemptedViewport_;
    QSize presentedViewport_;
    std::uint64_t presentedGeometryGeneration_{};
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

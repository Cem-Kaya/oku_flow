#pragma once

#include "okuflow/common/view_transform.hpp"
#include "okuflow/common/uct_device_image.hpp"
#include "okuflow/common/uct_model_settings.hpp"
#include "okuflow/common/uct_timing_trace.hpp"

#include <QColor>
#include <QByteArray>
#include <QImage>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QVector>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

// Ultra Clear Text (UCT): local OCR of stable printed camera text, accepted
// only after provenance, temporal-agreement and pixel-residual checks, and
// drawn as sharp glyph plates registered to the processed camera scene.
//
// Coordinate contract. Every recognized box is converted once from the pixels
// of the viewport copy it came from into normalized *processed-scene*
// coordinates (the same space as annotations: after rotation, stabilization
// and keystone, before zoom/pan/Fit/Fill/SuperRes sampling). Rendering maps
// scene coordinates through the current ViewTransform into normalized view
// coordinates and then into the overlay's logical pixels, so DPI is applied
// exactly once and the SuperRes ROI remap never enters text geometry.
// Recognition and validation consume independently owned, stamped CPU/GPU
// analysis images on private workers, never the D3D presentation stream.
namespace okuflow::uct {

// Immutable provenance of one owned CPU/GPU analysis image. Copies, jobs,
// results and accepted words all carry the stamp of the pixels they came
// from, never a newly sampled one.
struct SourceStamp {
    std::uint64_t sourceEpoch{};        // camera session generation
    std::uint64_t geometryGeneration{}; // processed-scene geometry version
    std::uint64_t frameSeq{};           // camera frame presented in the copy
    std::int64_t timestampMs{};         // monotonic time of the readback request
    ViewTransform viewTransform;        // full-scene -> viewport of the copy
    std::uint32_t viewportWidth{};
    std::uint32_t viewportHeight{};
    // Set only by the live-camera controller from capture-owned frame identity.
    // Selected media leaves this false; job/timestamp changes cannot set it.
    bool liveCameraCapture{};
    // OCR-input provenance copied from the paired device image. Validation
    // probes always describe the single exact frameSeq frame; these fields
    // disclose that the recognized pixels were a neutral temporal blend.
    bool temporallySmoothed{};
    std::uint64_t historyEpoch{};
    std::uint64_t seedFrameSeq{};
    std::uint64_t blendFrameCount{};
    float temporalAlpha{};
    // Extra Stable identity of the pixels, set only by a worker from the
    // capture's own device record (or Unknown while that is unresolved).
    // Held pixels belong to the earlier holdAcquisition: repeating one held
    // image is one observation. Crossfade/Unknown images are never evidence.
    HoldDisplay holdDisplay{HoldDisplay::NotHeld};
    std::uint64_t holdAcquisition{};

    bool SameScene(const SourceStamp& other) const noexcept
    {
        return sourceEpoch == other.sourceEpoch &&
               geometryGeneration == other.geometryGeneration;
    }
};

// The acquisition whose pixels a stamped image actually shows: frameSeq,
// or the earlier held acquisition. Composite/unknown pixels name none.
std::optional<std::uint64_t> EvidenceAcquisition(const SourceStamp& stamp);
// Metadata adapter only: the opt-in trace never feeds authority/admission.
inline timing::Key TimingKey(std::uint64_t producer, timing::Channel channel,
                            const SourceStamp& stamp, std::uint64_t jobId, std::uint64_t authority)
{
    return {producer, stamp.sourceEpoch, stamp.geometryGeneration, stamp.frameSeq,
            EvidenceAcquisition(stamp).value_or(0), jobId, authority, stamp.timestampMs, channel};
}
inline void TraceTiming(timing::Event event, std::uint64_t producer, timing::Channel channel,
                        const SourceStamp& stamp, std::uint64_t jobId, std::uint64_t authority,
                        timing::Values values = {}) noexcept
{
    if (!timing::Enabled()) return;
    try {
        if (stamp.liveCameraCapture) values.flags |= timing::LiveCamera;
        if (EvidenceAcquisition(stamp)) values.flags |= timing::AcquisitionKnown;
        timing::RecordEvent(event, TimingKey(producer, channel, stamp, jobId, authority), values);
    } catch (...) {} // diagnostics cannot affect recognition, including allocation faults
}
// Applies a worker-read device record. Inconsistent identity (live other than
// frameSeq, held not strictly older than frameSeq) becomes Unknown.
void ApplyHoldProvenance(SourceStamp& stamp, const HoldProvenance& hold);

// Normalized-scene <-> pixel mapping through one ViewTransform. `width` and
// `height` are the pixel extent of whatever surface the transform targets.
bool ViewPixelToScene(const ViewTransform& transform, double width, double height,
                      const QPointF& viewPixel, QPointF& scenePoint);
QPointF SceneToViewPixel(const ViewTransform& transform, double width, double height,
                         const QPointF& scenePoint);
QRectF ViewRectToScene(const ViewTransform& transform, double width, double height,
                       const QRectF& viewRect);
QRectF SceneRectToView(const ViewTransform& transform, double width, double height,
                       const QRectF& sceneRect);

// Reading-order text quadrilateral: top-left, top-right, bottom-right and
// bottom-left along the recognized baseline. Paddle detector lines and their
// CTC word spans are rotated rectangles in source pixels. Scene coordinates
// keep the four mapped points, so the anisotropic normalized scene mapping
// and the final view transform remain exact affine maps of the same shape.
// An absent quad (valid == false) means the axis-aligned rectangle applies.
struct TextQuad {
    std::array<QPointF, 4> points{};
    bool valid{};

    static TextQuad FromRect(const QRectF& rect);
    QRectF Bounds() const;
    QPointF Center() const;
    TextQuad Translated(const QPointF& offset) const;
    bool operator==(const TextQuad&) const = default;
};
// Length of the baseline (top edge) and of the left edge, in the units of
// the coordinates. Exact for rotated rectangles in isotropic pixel space.
double QuadWidth(const TextQuad& quad);
double QuadHeight(const TextQuad& quad);
// Baseline angle in radians; meaningful only in isotropic pixel space.
double QuadAngle(const TextQuad& quad);
// Pixel-space quads tilted at least this much keep oriented geometry through
// ink fitting and paint. Smaller tilts retain the established rectangle path.
inline constexpr double kOrientedTextMinimumRadians = 0.035; // about 2 degrees
bool QuadIsRotated(const TextQuad& pixelQuad);
// Exact convex intersection area (Sutherland-Hodgman); 0 for invalid input.
double QuadArea(const TextQuad& quad);
double QuadIntersectionArea(const TextQuad& a, const TextQuad& b);
TextQuad ViewQuadToScene(const ViewTransform& transform, double width, double height,
                         const TextQuad& viewQuad);
TextQuad SceneQuadToView(const ViewTransform& transform, double width, double height,
                         const TextQuad& sceneQuad);

// Owned BGRA pixels handed from the UI thread to workers without copying.
// After hand-off nobody writes to the buffer; workers only read it.
struct PixelBuffer {
    std::vector<std::uint8_t> bytes;
    int width{};
    int height{};
    int strideBytes{};
    bool IsValid() const noexcept
    {
        return width > 0 && height > 0 && strideBytes >= width * 4 &&
               bytes.size() >= static_cast<std::size_t>(strideBytes) * static_cast<std::size_t>(height);
    }
};
using SharedPixels = std::shared_ptr<const PixelBuffer>;

// Recycles viewport-sized byte vectors so the steady-state hand-off of a
// presenter readback is a vector swap, never a 4K copy or allocation. A
// buffer returns to the pool when its last owner (UI, probe worker or OCR
// worker) releases it, on whichever thread that happens.
class PixelPool : public std::enable_shared_from_this<PixelPool> {
public:
    static std::shared_ptr<PixelPool> Create(int capacity = 4);
    // Takes ownership of `bytes` (e.g. the presenter's reusable readback
    // vector) and refills it with a recycled vector when one is free.
    SharedPixels Adopt(std::vector<std::uint8_t>& bytes, int width, int height, int strideBytes);
    // Copies caller-owned pixels into a pooled buffer.
    SharedPixels Copy(const std::uint8_t* bgra, int width, int height, int strideBytes);
    int FreeCount() const;
    std::uint64_t Allocations() const;

private:
    explicit PixelPool(int capacity) : capacity_(capacity) {}
    SharedPixels Wrap(PixelBuffer* buffer);
    std::vector<std::uint8_t> Take(std::size_t minimumSize);
    void Give(std::vector<std::uint8_t>&& bytes);

    mutable std::mutex mutex_;
    std::vector<std::vector<std::uint8_t>> free_;
    int capacity_{};
    std::uint64_t allocations_{};
};

// QImage view of shared pixels; the image keeps the buffer alive.
QImage WrapPixels(const SharedPixels& pixels);

class ResidentProbe;
class ResidentPatch { public: virtual ~ResidentPatch() = default; };

// CPU luma/integral or an independently owned GPU validation surface.
// The image can be any size; mapping uses the stamp's normalized transform.
struct ProbeFrame {
    SourceStamp stamp;
    QImage luma;                // Format_Grayscale8
    QVector<quint32> integral;  // summed-area table, (w + 1) x (h + 1)
    std::shared_ptr<const ResidentProbe> resident;
    int Width() const noexcept;
    int Height() const noexcept;
    bool IsValid() const noexcept
    {
        if (resident) return stamp.viewTransform.valid && Width() >= 8 && Height() >= 8;
        return !luma.isNull() && stamp.viewTransform.valid &&
               integral.size() == (luma.width() + 1) * (luma.height() + 1);
    }
};

// Builds a luma probe from a BGRA/ARGB32 viewport copy, decimating by an
// integer factor (>= 1). Returns an invalid probe for malformed input.
ProbeFrame MakeProbeFrame(const std::uint8_t* bgra, int width, int height, int strideBytes,
                          int decimation, const SourceStamp& stamp);

struct ReferencePatch {
    QRectF sceneRect; // sampled area (line box plus margin), scene units
    int columns{};
    int rows{};
    QVector<float> values; // CPU zero-mean, unit-variance samples
    bool valid{};
    std::shared_ptr<const ResidentPatch> resident; // GPU descriptor, no pixel/feature download
    TextQuad sceneQuad; // optional rectified sampling frame for immutable word proof
};

// Metadata-only bounded sampling plan; no image access or independent evidence.
ReferencePatch PlanQuadReference(const SourceStamp&, int width, int height, const TextQuad&);
ReferencePatch BuildQuadReferencePatch(const ProbeFrame&, const TextQuad&);

// --- Recognition backend -------------------------------------------------

enum class BackendState : std::uint8_t {
    kNotStarted,
    kReady,
    kUnavailable, // runtime cannot recognize (e.g. no OCR language installed)
    kFailed,      // backend initialized but stopped working
};

struct BackendStatus {
    BackendState state{BackendState::kNotStarted};
    QString name;        // e.g. "Windows.Media.Ocr"
    QString languageTag; // recognizer language actually used
    QString detail;      // English technical detail for diagnostics
    bool usesGpu{};      // actual selected recognizer, including temporary fallback
    // The selected recognizer can restrict detection to one visible-detail
    // region of the same owned image (Paddle CPU/TensorRT, not Windows OCR).
    bool detailFocus{};
};

// One bounded visible-detail detection region of an already owned analysis
// image. It never changes the image, stamp or acquisition: recognition still
// samples the same source pixels and maps boxes to full-image pixels.
struct DetailFocus {
    QRect regionPx;                    // requested visible source region
    double maximumMagnification{1.0};  // detector enlargement bound, clamped to [1, 6]
    int detectorLongEdge{960};         // [32, 960]; supplemental regions use 480
    bool supplemental{};              // failure requests a later full read, not backend retirement
};
// Hard bound on visible-detail detector enlargement. Enlarging existing pixels
// adds no source detail; it only gives the detector more pixels per glyph.
inline constexpr double kMaximumDetailMagnification = 6.0;
// Detector boxes this close to an internal crop edge are incomplete. Shared
// by backend merging and regional ownership (which adds detector jitter slack).
inline constexpr double kDetailCropEdgeMarginPx = 8.0;

// Backend-neutral detector input planning, shared by the Paddle CPU/TensorRT
// adapters and the controller's visible-detail decision.
// Input size for one detector region: long edge <= 960, enlargement only up
// to the clamped magnification (1 = never enlarge, the established passes).
QSize DetectorInputSize(QSize region, double maximumMagnification = 1.0, int maximumLongEdge = 960);
// Whole-scene context plus at most 4x4 overlapping crops (desired edge 1280,
// 160-pixel overlap). The 4 is a per-axis crop count, not a zoom factor.
QVector<QRect> DetectorRegions(QSize source);
double DetailMagnification(const DetailFocus& focus);
// The single visible-detail region: the request clamped to the image and,
// when its long edge is below 960/magnification, enlarged around its centre
// so detector enlargement never exceeds the bound. Empty for unusable input.
QRect DetailRegion(QSize source, const DetailFocus& focus);
// Highest detector scale (input pixels per source pixel) any established
// region gives at `point`; the baseline a detail pass must clearly beat.
double EstablishedDetectorScale(QSize source, const QPointF& point);

// Candidate text is evidence, not accepted reading text. Rejected spans stay
// ordered through preparation/admission and protect their original pixels.
enum class TokenDisposition : std::uint8_t {
    kCandidate,
    kLowConfidence,
    kTooSmall,
    kHighRisk,
    kInvalidGeometry,
    kUnrecognized,
};

struct LineColors {
    QColor foreground{Qt::black};
    QColor background{Qt::white};
};

struct RecognizedWord {
    QString text;
    QRectF boundsPx; // pixels of the job image
    bool paddedBounds{}; // detector/CTC region needs worker-side ink fitting
    QRectF inkSearchPx; // optional source search, bounded by the line/neighbours
    TokenDisposition disposition{TokenDisposition::kCandidate};
    double confidence{1.0}; // backend evidence; never increased during admission
    QRectF protectedBoundsPx; // original detector span, before optional ink fitting
    std::optional<LineColors> colors; // local word surface, measured after recognition
    // Oriented Paddle geometry in job pixels. boundsPx is always its
    // axis-aligned bound. Windows OCR words keep valid == false.
    TextQuad quadPx;          // CTC span over the line height; fitted ink after preparation
    TextQuad inkSearchQuadPx; // span widened by one CTC step, bounded by neighbours
    bool inkFitted{};         // worker ink fitting replaced the padded span
};

struct RecognizedLine {
    QVector<RecognizedWord> words;
};

struct RecognitionOutput {
    bool ok{};
    bool cancelled{};
    QString error;
    QVector<RecognizedLine> lines;
    bool inputFault{}; // dropped frame/copy, not a reason to retire a backend
    bool backendPreparing{}; // preferred backend pending; complete fallback output may still be ok
    // Extra Stable crossfade/unknown input: recognition was not attempted.
    // Neither a backend nor an input fault, and never recognition evidence.
    bool compositeInput{};
    // Non-empty when detection examined only this region of the same image
    // (visible detail). Boxes remain full-image pixels. Words touching an
    // internal region edge were excluded, never cut into fragments.
    QRect detailRegionPx;
};

// Runs only on the scheduler's worker thread. Implementations must honor
// `cancelled` promptly where the platform allows, but may finish the native
// call; the scheduler keeps the input image alive until Recognize returns.
class Recognizer {
public:
    virtual ~Recognizer() = default;
    virtual BackendStatus Initialize() = 0;
    // Optional runtime status update after automatic backend failover.
    virtual BackendStatus Status() const { return {}; }
    virtual bool WantsDeviceInput() const { return false; }
    virtual bool Preparing() const { return false; }
    // Worker-confined: the scheduler sets this immediately before one
    // recognition and clears it afterwards. Backends without detail support
    // ignore it and leave RecognitionOutput::detailRegionPx empty.
    virtual void SetDetailFocus(const std::optional<DetailFocus>& focus) { Q_UNUSED(focus); }
    virtual RecognitionOutput Recognize(const QImage& image,
                                        const std::atomic<bool>& cancelled) = 0;
    virtual RecognitionOutput RecognizeDevice(const DeviceImage& image,
                                              const std::atomic<bool>& cancelled)
    {
        QString error;
        const QImage pixels=image.Readback(cancelled,error);
        if(pixels.isNull()) {RecognitionOutput output;output.error=error;output.cancelled=cancelled.load();output.inputFault=true;return output;}
        return Recognize(pixels,cancelled);
    }
};

// Optional TensorRT -> Paddle CPU -> built-in Windows OCR, selected on worker.
std::unique_ptr<Recognizer> CreateDefaultRecognizer(const ModelSettings& models = {});

// --- Scheduler -------------------------------------------------------------

// A recognized line already mapped to scene coordinates with its residual
// reference, prepared on the recognition worker so admission on the UI
// thread does no pixel work.
struct PreparedWord {
    QString text;
    QRectF sceneRect;
    TokenDisposition disposition{TokenDisposition::kCandidate};
    double confidence{1.0};
    QRectF protectedSceneRect;
    std::optional<LineColors> colors;
    TextQuad sceneQuad; // oriented scene geometry of a rotated word; sceneRect is its bound
};

// Worker-side oriented ink fit, the rotated-frame counterpart of the
// row-band rectangle fit. Samples the word's search quad on a bounded local
// grid, keeps the dominant ink band (plus detached marks) and returns the
// fitted quad, or the unchanged span when ink is ambiguous. CPU reference
// for the resident CUDA kernel; never called on the UI thread.
TextQuad FitOrientedWordInk(const QImage& image, const RecognizedWord& word,
                            const LineColors& colors, bool* fitted = nullptr);
// Worker-side completion after fitting, shared by CPU and resident paths:
// rejects invalid convex geometry and removes duplicate observations of the same text
// that fitting resolved onto the same ink. Lines are never removed, so
// every parallel per-line vector stays aligned.
void FinalizeRecognizedGeometry(RecognitionOutput& output);

struct PreparedLine {
    QVector<PreparedWord> words; // every known span, including rejected ones
    QRectF sceneRect;
    ReferencePatch reference;    // invalid when untrackable
};

struct MatchTuning {
    double minimumScore{0.82};
    double minimumVisibleFraction{0.85};
    int searchRadiusPx{6}; // probe pixels, clamped to [0, 10]; zero disables translation/refinement
};

// OCR/current halves have identical coordinates. Unlike later motion tracking,
// input registration must not search for old words elsewhere in the new frame.
inline constexpr MatchTuning kInputRegistrationTuning{0.94, 0.95, 0};

// Worker registration is tied to one selected job and the exact model policy.
struct CorroborationRequest {
    std::uint64_t selectedJobId{};
    QVector<ReferencePatch> references;
    MatchTuning tuning;
    // Immutable selected-frame evidence, shared with the neighbour worker.
    // Used only for word-local registration when scrolling defeats line motion.
    ProbeFrame selectedReference;
    QVector<PreparedLine> selectedLines;
};

struct RegionShadowPlan;
// One optional descriptor in the existing latest-only slot. Deadlines use
// steady wall time, never source PTS or a synthetic acquisition. The worker
// rechecks the exact retained full plan, deadline and pending full priority.
struct RegionalOcrRequest {
    std::shared_ptr<const RegionShadowPlan> plan;
    std::chrono::steady_clock::time_point fullDeadline;
    double estimatedMs{750.0};
};
enum class RecognitionJobKind : std::uint8_t { Full, PausedDetail, Regional };
struct RecognitionResult {
    std::uint64_t jobId{};
    std::uint64_t authority{};
    SourceStamp stamp;
    RecognitionOutput output;
    // Single-frame luma reference at the job's latest contributing frame.
    // Smoothed OCR additionally requires currentInputMatches below.
    ProbeFrame reference;
    QVector<LineColors> lineColors; // parallel to output.lines
    // For smoothed OCR, every candidate word must match in place with stricter
    // registration tuning. One worker-computed verdict per line compares the
    // recognized smoothed pixels and the exact current frame. Merely matching
    // a raw reference against later raw probes cannot prove OCR correspondence.
    QVector<bool> currentInputMatches;
    // Parallel to output.lines when filled by the scheduler; Admit prepares
    // lines itself when this is empty (hand-built fixtures).
    QVector<PreparedLine> prepared;
    double durationMs{};
    bool deviceInput{};
    QByteArray sourcePixelDigest; // complete saved/unknown input fingerprint; empty for camera/probes
    // Digests from different algorithms are never independent evidence.
    enum class DigestKind { Sha256, GpuTreeSha256V1 };
    DigestKind sourcePixelDigestKind{DigestKind::Sha256};
    QVector<bool> corroborationMatches; // exactly one per selected prepared line
    QVector<QVector<bool>> corroborationWordMatches; // selected line/word indices; no display offsets
    std::uint64_t corroboratedJobId{};
    MatchTuning corroborationTuning;
    // Full-output planner metadata; active scheduling may consume it, never
    // overlay/UIA admission or proof. Partial outputs do not observe it.
    std::shared_ptr<const RegionShadowPlan> shadowPlan;
    double shadowPlannerMs{}; // excluded from ordinary OCR worker duration
    QString shadowPlannerError;
    RecognitionJobKind jobKind{RecognitionJobKind::Full}; // actual backend operation
    bool regionalRequested{};
    bool regionalFallback{}; // stale/unsupported/no slack: full, or yielded to queued full
    bool regionalFailed{};   // request a full read without retiring healthy backend/text
    bool regionalDeadlineOverrun{};
};

// Maps recognized words into scene coordinates and builds per-line residual
// references from `result.reference`. CPU work is pure; resident input is worker-only.
QVector<PreparedLine> PrepareLines(const RecognitionResult& result, double minimumWordHeightPx);

struct SchedulerStats {
    std::uint64_t submitted{};
    std::uint64_t coalesced{}; // pending jobs replaced before starting
    std::uint64_t started{};
    std::uint64_t completed{};
    std::uint64_t revoked{};   // finished after their authority was cancelled
    std::uint64_t fullCalls{}, pausedDetailCalls{}, regionalCalls{};
    std::uint64_t regionalAttempts{}; // SetDetailFocus call attempted, including ignored/full responses
    std::uint64_t regionalFallbacks{}, regionalFailures{}, regionalDeadlineOverruns{};
    std::uint64_t regionalPriorityRefusals{};
    double maximumFullMs{}, maximumRegionalMs{}; // worker call through preparation
};

// One worker thread, at most one job in flight plus one latest-only pending
// slot. Submit never blocks on recognition. Cancel() revokes authority of
// pending and in-flight work immediately; the in-flight input copy stays
// owned by the worker until the native call returns. Destruction waits a
// bounded time and otherwise leaves the worker (and its owned inputs) to
// retire on its own, never freeing memory still in use.
class RecognitionScheduler {
public:
    using Completion = std::function<void(RecognitionResult)>; // worker thread
    using StatusChanged = std::function<void(BackendStatus)>;   // worker thread

    RecognitionScheduler(std::unique_ptr<Recognizer> recognizer,
                         Completion completion,
                         StatusChanged statusChanged,
                         int referenceDecimation = 2,
                         double minimumWordHeightPx = 7.0,
                         bool probeOnly = false,
                         bool shadowPlanner = false,
                         bool regionalOcr = false);
    ~RecognitionScheduler();
    RecognitionScheduler(const RecognitionScheduler&) = delete;
    RecognitionScheduler& operator=(const RecognitionScheduler&) = delete;

    // Takes an independently owned copy (detached by the caller or here).
    // Returns the job id, or 0 when the backend is unavailable/failed.
    // `detail` restricts detection to one bounded region of the SAME owned
    // image. The job keeps the image's stamp, digest and acquisition: it is
    // a refinement of that observation, never independent evidence.
    std::uint64_t Submit(const SourceStamp& stamp, QImage pixels,
                         CorroborationRequest corroboration = {},
                         std::optional<DetailFocus> detail = std::nullopt,
                         std::optional<RegionalOcrRequest> regional = std::nullopt);
    // Zero-copy hand-off of already shared, immutable pixels.
    std::uint64_t Submit(const SourceStamp& stamp, SharedPixels pixels,
                         CorroborationRequest corroboration = {},
                         std::optional<DetailFocus> detail = std::nullopt,
                         std::optional<RegionalOcrRequest> regional = std::nullopt);
    std::uint64_t SubmitDevice(const SourceStamp& stamp, SharedDeviceImage device,
                               CorroborationRequest corroboration = {},
                               std::optional<DetailFocus> detail = std::nullopt,
                               std::optional<RegionalOcrRequest> regional = std::nullopt);
    void Cancel();
    // Overwrites one optional priority descriptor for the next job only.
    // Does not submit work, reset/renew evidence or alter recognition policy.
    void SetShadowFocus(std::optional<QRectF> sceneRect);
    // Explicit hardware-free fixture seam; affects only an enabled diagnostic
    // observer. Recognition and its original-image fallback remain unchanged.
    void FailNextShadowObservationForTest();
    // One-shot hardware-free fixture barrier, outside the scheduler mutex,
    // before taking the pending job. Normal workers have no barrier.
    void SetBeforeDequeueForTest(std::function<void()> barrier);
    std::uint64_t Authority() const;
    bool IsCurrent(const RecognitionResult& result) const;
    bool HasWork() const; // pending or in flight
    bool InFlight() const;
    BackendStatus Status() const;
    SchedulerStats Stats() const;
    std::uint64_t TimingProducerId() const noexcept;

private:
    struct State;
    std::shared_ptr<State> state_;
};

// --- Acceptance and registration ------------------------------------------

ReferencePatch BuildReferencePatch(const ProbeFrame& reference, const QRectF& sceneRect);

struct PatchMatch {
    bool matched{};
    bool visible{};        // enough of the patch lies inside the probe
    double score{};        // normalized cross-correlation
    QPointF sceneOffset;   // validated translation, scene units
};

// Independent GPU probe lifetime; never pins a full AnalysisPool slot. Methods
// run only on OCR/probe workers. The UI consumes their small returned records.
class ResidentProbe {
public:
    virtual ~ResidentProbe() = default;
    virtual int Width() const noexcept = 0;
    virtual int Height() const noexcept = 0;
    virtual ReferencePatch Reference(const SourceStamp&, const QRectF&) const = 0;
    virtual PatchMatch Match(const SourceStamp&, const ReferencePatch&, const MatchTuning&,
                              std::optional<QPointF>) const = 0;
    virtual std::optional<double> Similarity(const SourceStamp&, const ResidentProbe&,
                                             const SourceStamp&) const = 0;
    // Native CUDA batches one block per line and returns one metadata array.
    // Defaults retain compatibility with hardware-free protocol oracles.
    virtual QVector<ReferencePatch> References(const SourceStamp& stamp, const QVector<QRectF>& rects) const {
        QVector<ReferencePatch> out;out.reserve(rects.size());
        for(const auto& rect:rects)out.push_back(Reference(stamp,rect));
        return out;
    }
    // Oracles without oriented sampling fail closed, rather than pretending a
    // bounding rectangle contains the same pixels as a rotated word.
    virtual QVector<ReferencePatch> QuadReferences(const SourceStamp&, const QVector<TextQuad>& quads) const {
        return QVector<ReferencePatch>(quads.size());
    }
    virtual QVector<PatchMatch> Matches(const SourceStamp& stamp,const QVector<ReferencePatch>& patches,
                                        const MatchTuning& tuning,const QVector<QPointF>& seeds,
                                        const std::atomic<bool>* cancelled = nullptr) const {
        QVector<PatchMatch> out;out.reserve(patches.size());
        for(int i=0;i<patches.size();++i){
            if(cancelled&&cancelled->load())return {};
            const auto seed=i<seeds.size()&&seeds[i]!=QPointF()?std::optional<QPointF>(seeds[i]):std::nullopt;
            out.push_back(Match(stamp,patches[i],tuning,seed));
        }
        return out;
    }
};

// `seedSceneOffset` (e.g. the line's last validated offset) is tried before
// the coarse grid; it never widens the bounded search window.
PatchMatch MatchPatch(const ReferencePatch& patch, const ProbeFrame& probe,
                      const MatchTuning& tuning = {},
                      std::optional<QPointF> seedSceneOffset = std::nullopt);

// One accepted line to revalidate, snapshotted for a worker thread.
// ReferencePatch values are implicitly shared and never mutated after
// creation, so the snapshot is a reference-count copy.
struct LineReference {
    std::uint64_t lineId{};
    ReferencePatch reference;
    QPointF lastSceneOffset;
};

struct LineCheck {
    std::uint64_t lineId{};
    bool checked{}; // false: budget exhausted or cancelled, so unverified
    PatchMatch match;
};

// Residual-checks lines against a probe until `budgetMs` elapses (<= 0 is
// unlimited) or `cancelled` is set. CPU checks stop between lines; resident
// GPU checks stop between bounded 64-line batches, preserving completed checks.
// Unchecked lines are reported, never silently treated as valid.
QVector<LineCheck> CheckLines(const QVector<LineReference>& lines, const ProbeFrame& probe,
                              const MatchTuning& tuning, double budgetMs,
                              const std::atomic<bool>* cancelled = nullptr);

// Scene-registered similarity of two probes over their common area. Returns
// a correlation in [-1, 1], or nullopt when they share too little area.
std::optional<double> SceneSimilarity(const ProbeFrame& a, const ProbeFrame& b);

// True for tokens whose characters are commonly confused (0/O, 1/l/I, 5/S,
// 8/B, 2/Z) when letters and digits mix in one word, or which contain
// notation the MVP never claims (superscripts, math operators beyond simple
// arithmetic). Such words fail closed to original pixels.
bool IsHighRiskToken(const QString& text);

// --- Probe worker -------------------------------------------------------------

// Everything one probe needs, owned by the request: the viewport pixels, the
// previous probe for scene stability, and snapshots of the lines to check.
struct ProbeRequest {
    SourceStamp stamp;
    SharedPixels pixels;
    std::shared_ptr<const ProbeFrame> preparedProbe;
    int decimation{2};
    std::shared_ptr<const ProbeFrame> previous;
    QVector<LineReference> lines;
    MatchTuning tuning;
    double budgetMs{60.0}; // residual checks stop here; the rest stay unverified
};

struct ProbeOutcome {
    std::uint64_t jobId{};
    std::uint64_t authority{};
    SourceStamp stamp;
    SharedPixels pixels; // handed back for a possible zero-copy OCR submit
    std::shared_ptr<const ProbeFrame> probe; // null when the pixels were unusable
    std::optional<double> similarity;        // against `previous`
    QVector<LineCheck> checks;
    int uncheckedLines{};
    double probeMs{};
    double checkMs{};
};

struct ProbeWorkerStats {
    std::uint64_t submitted{};
    std::uint64_t coalesced{}; // pending requests replaced before starting
    std::uint64_t completed{};
    std::uint64_t revoked{};
    double maxCheckMs{};
};

// Builds probes and runs residual validation off the UI thread. One worker,
// at most one request in flight plus one latest-only pending slot; Submit
// never blocks on work. Cancel() revokes authority immediately and stops the
// in-flight check loop at the next line. CPU checks and resident GPU operations
// are worker-only; destruction waits a bounded time, then leaves held inputs
// with the callback-revoked worker.
class ProbeWorker {
public:
    using Completion = std::function<void(ProbeOutcome)>; // worker thread

    explicit ProbeWorker(Completion completion);
    ~ProbeWorker();
    ProbeWorker(const ProbeWorker&) = delete;
    ProbeWorker& operator=(const ProbeWorker&) = delete;

    std::uint64_t Submit(ProbeRequest request);
    void Cancel();
    bool IsCurrent(const ProbeOutcome& outcome) const;
    bool HasPending() const;
    bool HasWork() const;
    ProbeWorkerStats Stats() const;
    std::uint64_t TimingProducerId() const noexcept;

private:
    struct State;
    std::shared_ptr<State> state_;
};

enum class AdmitVerdict : std::uint8_t {
    kAccepted,      // at least one word now displayed
    kPending,       // awaiting temporal agreement
    kStaleScene,    // source/geometry changed since the copy
    kTooOld,        // arrived after the age gate
    kContentMoved,  // newer probe no longer matches the recognized pixels
    kFailed,        // backend error or cancelled
    kEmpty,         // no usable text
};

struct AcceptedWord {
    QString text;
    QRectF sceneRect;
    bool glyphEligible{true}; // immutable text may be readable without corroboration
    std::optional<LineColors> colors; // absent in legacy/hand-built line-only evidence
    TextQuad sceneQuad; // oriented geometry (before line offset); invalid for rectangles
    std::uint64_t layoutGroupId{}; // typography group; 0 uses the owning line id
};

struct AcceptedLine {
    std::uint64_t id{};
    QRectF sceneRect;
    QVector<AcceptedWord> words;   // confirmed words only
    int omittedWords{};            // unconfirmed / high-risk words left original
    QVector<QString> readingTokens; // original order; empty token marks an omission
    QVector<QRectF> protectedSceneRects; // unconfirmed/rejected source ink, before offset
    LineColors colors;
    ReferencePatch reference;
    SourceStamp provenance;        // stamp of the pixels the text came from
    // Validation state (updated by probes).
    bool valid{};
    QPointF sceneOffset;
    std::int64_t validatedAtMs{};
    std::uint64_t validatedFrameSeq{};
    // Empty uses the original sequential readingTokens/words contract. An
    // immutable refinement may append independently proved words while old
    // word indexes remain stable; then every token has an explicit index
    // (-1 for an omission), and unused reading-only words stay undrawn.
    QVector<int> readingWordIndices;
    // Subset of protectedSceneRects naming semantic-risk/invalid-geometry
    // ink. Immutable refinement may not carve these masks. Ordinary grouped
    // prose can still be replaced by separately proved word tokens.
    QVector<QRectF> nonCarvableSceneRects;
};

struct SceneWord {
    QString text;
    QRectF sceneRect; // validated offset applied
    LineColors colors;
    std::uint64_t lineId{};
    QVector<QRectF> protectedSceneRects; // all current protected spans, offset applied
    int wordIndex{-1}; // index into the owning AcceptedLine::words (accessibility key)
    TextQuad sceneQuad; // oriented word geometry, offset applied; invalid for rectangles
    std::uint64_t layoutGroupId{}; // independent of reading identity; 0 uses lineId
};

// One token of a screen-reader line projection: a verified glyph word, or a
// single collapsed run of spans left as the original image ("…").
struct ScreenToken {
    int wordIndex{-1}; // AcceptedLine::words index; -1 marks the "…" run
    int start{};       // UTF-16 offset into ScreenLine::text
    int length{};      // UTF-16 code units
};

// Text of one shown line exactly as the overlay replaces it: glyph-eligible
// words joined by single spaces, every omission, rejected span and
// reading-only word collapsed into one "…" per run. Never contains '\n'.
struct ScreenLine {
    std::uint64_t lineId{};
    QString text;
    QVector<ScreenToken> tokens; // ordered, non-overlapping, separated by one space
    QRectF sceneRect;            // line box with the validated offset applied
    bool truncated{};            // per-line cap reached; a trailing "…" was added
};

// Resource bounds for ScreenLines(); overflow is cut at the end of reading
// order. maxTotalUtf16 counts UTF-16 code units (8192 units = 16 KiB).
struct ScreenTextLimits {
    int maxLines{128};
    int maxLineUtf16{512};
    int maxTotalUtf16{8192};
};

// Deterministic reading order shared by the reading pane and screen text:
// lines are clustered into rows by vertical centre (a line joins the row
// while its centre lies inside the row's first line), rows run top to
// bottom, and lines inside a row left to right (ties: line id).
QVector<const AcceptedLine*> OrderLinesForReading(QVector<const AcceptedLine*> lines);

struct DisplayWord {
    QString text;
    QRectF viewRect; // pixels of the requested surface
    LineColors colors;
    std::uint64_t lineId{};
};

struct ModelTuning {
    std::int64_t maxResultAgeMs{4000};
    std::int64_t freshnessMs{350};
    double minimumWordHeightPx{7.0};
    double agreementIou{0.5};
    // UI-thread budget for checking a result against a newer probe. Lines not
    // reached stay hidden until the next off-thread probe validates them.
    double admitMatchBudgetMs{3.0};
    MatchTuning match;
};

struct ValidationSummary {
    int checked{};
    int valid{};
    int invalid{};
    int notVisible{};
    int unchecked{}; // budget exhausted: unverified, hidden
};

class OverlayModel {
public:
    explicit OverlayModel(ModelTuning tuning = {});

    void Reset();
    // A previously pending live word needs a different frame and unequal
    // complete source-pixel digests under the same scheduler authority.
    // Confirmed words may survive a duplicate refresh without new evidence.
    AdmitVerdict Admit(const RecognitionResult& result,
                       const SourceStamp& current,
                       const ProbeFrame* latestProbe,
                       std::int64_t nowMs);
    // Live capture uses the capture owner's monotonically increasing sample
    // sequence as independent acquisition evidence. Identical source pixels
    // are valid on a steady camera/virtual feed; repainting the same sample
    // never supplies a second observation. Capture provenance, scheduler
    // authority, scene/age, agreement and pixel-residual checks still apply;
    // the live worker does not compute a redundant full-image digest.
    // Extra Stable held images count as their earlier held acquisition, never
    // as the newer presentation sequence; a single held observation is shown
    // only as reading text that leaves the original ink in place.
    AdmitVerdict AdmitLiveCamera(const RecognitionResult& result,
                                const SourceStamp& current,
                                const ProbeFrame* latestProbe,
                                std::int64_t nowMs);
    // Advancing saved media retains real media PTS in every stamp. Admission
    // uses the live temporal/residual policy with an independent caller-owned
    // wall-clock job deadline instead of the live source-age gate.
    AdmitVerdict AdmitAdvancingMedia(const RecognitionResult& result,
                                     const SourceStamp& current,
                                     const ProbeFrame* latestProbe,
                                     std::int64_t mediaNowMs);
    // One explicitly selected immutable frame: require exact scene/frame
    // identity. One result exposes text with uncertainty and leaves source ink.
    // Glyphs require an optional genuinely distinct compatible neighbor: full
    // pixel digests, different frame identity, word agreement and registration.
    // Caller owns selected-media/model authority; no periodic probe is needed.
    // A refinement of identical selected pixels preserves exact earlier proof
    // and can add independently proved words; it never rebinds old proof to
    // jittered recognition boxes or another selected input.
    AdmitVerdict AdmitImmutable(const RecognitionResult& result,
                                const SourceStamp& selectedFrame,
                                std::int64_t nowMs,
                                const RecognitionResult* corroboration = nullptr);
    // Revalidates every accepted line against a newer probe of the same scene
    // (synchronous: CheckLines + ApplyChecks).
    ValidationSummary Validate(const ProbeFrame& probe, std::int64_t nowMs);
    // Snapshots of lines of `scene` for an off-thread CheckLines.
    QVector<LineReference> LineReferences(const SourceStamp& scene) const;
    // Applies off-thread checks made against a probe stamped `probeStamp`.
    // Lines removed or re-admitted meanwhile are ignored; lines validated by
    // newer pixels than the probe keep that newer state. `renew` false (an
    // Extra Stable crossfade composite) revokes a matched line's display and may remove lines, but never
    // validates one or advances its proof clock.
    ValidationSummary ApplyChecks(const QVector<LineCheck>& checks, const SourceStamp& probeStamp,
                                  bool renew = true);
    // Words safe to draw now, in scene units. A line is shown only if its
    // latest validation is fresh, or no newer camera frame has been
    // presented since it.
    QVector<SceneWord> SceneWords(std::int64_t nowMs, std::uint64_t currentFrameSeq) const;
    // SceneWords projected through a transform onto a pixel surface.
    QVector<DisplayWord> DisplayWords(const ViewTransform& transform, double width,
                                      double height, std::int64_t nowMs,
                                      std::uint64_t currentFrameSeq) const;
    // Plain reading-order text with ordered omissions ("…"). Text with no
    // safe registration stays in this pane, prefixed by an omission marker,
    // while original pixels remain unchanged. Rows are clustered, then each row
    // is ordered left to right, so the order is deterministic and strict.
    QString ReadingText(std::int64_t nowMs, std::uint64_t currentFrameSeq) const;
    // Lines whose glyphs SceneWords() would draw now, projected for screen
    // readers in reading order (see ScreenLine). Lines whose reading tokens
    // and confirmed words disagree are left out rather than guessed.
    QVector<ScreenLine> ScreenLines(std::int64_t nowMs, std::uint64_t currentFrameSeq,
                                    const ScreenTextLimits& limits = {}) const;
    const QVector<AcceptedLine>& Lines() const noexcept { return lines_; }
    int AcceptedWordCount() const;
    int PendingWordCount() const noexcept { return pending_.size(); }
    bool HasAcceptedText() const;

    // Glyphs additionally require a valid registration reference. Reading-only
    // evidence shares the same source freshness but cannot renew from a probe.
    bool IsShown(const AcceptedLine& line, std::int64_t nowMs, std::uint64_t currentFrameSeq) const;
    bool IsReadable(const AcceptedLine& line, std::int64_t nowMs, std::uint64_t currentFrameSeq) const;

private:

    AdmitVerdict AdmitImpl(const RecognitionResult& result, const SourceStamp& current,
                           const ProbeFrame* latestProbe, std::int64_t nowMs, bool immutable,
                           const RecognitionResult* corroboration = nullptr,
                           bool advancingMedia = false, bool liveCamera = false);
    AdmitVerdict MergeImmutableCoverage(const QVector<AcceptedLine>& previous,
                                        AdmitVerdict verdict, std::int64_t nowMs,
                                        std::uint64_t selectedFrameSeq);
    bool ApplyImmutableConflictBlocks(QVector<AcceptedLine>& lines) const;

    struct PendingEvidence {
        SourceStamp stamp;
        QByteArray digest;
        RecognitionResult::DigestKind digestKind{RecognitionResult::DigestKind::Sha256};
        std::uint64_t authority{};
    };
    struct PendingWord {
        QString text;
        QRectF sceneRect;
        std::uint64_t sourceEpoch{};
        std::uint64_t geometryGeneration{};
        std::shared_ptr<const PendingEvidence> evidence;
    };

    ModelTuning tuning_;
    QVector<AcceptedLine> lines_;
    QVector<PendingWord> pending_;
    // Full reads replace pending observations; regional reads preserve outside
    // words with their exact shared source ticket. At most 8192 words/tickets,
    // no retained source images. These fields also name the immutable input.
    SourceStamp pendingEvidenceStamp_;
    QByteArray pendingEvidenceDigest_;
    RecognitionResult::DigestKind pendingDigestKind_{RecognitionResult::DigestKind::Sha256};
    std::uint64_t pendingEvidenceAuthority_{};
    std::uint64_t nextLineId_{1};
    // Exact selected-input disagreements keep only those source regions as
    // original ink, including after a later result omits the conflicting word.
    // Bounded independently of refinements; no history crosses input identity.
    QVector<QRectF> immutableConflictRects_;
    bool immutableConflictSaturated_{};
    bool immutable_{};
    bool liveCamera_{};
};

} // namespace okuflow::uct

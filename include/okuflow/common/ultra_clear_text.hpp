#pragma once

#include "okuflow/common/view_transform.hpp"
#include "okuflow/common/uct_device_image.hpp"
#include "okuflow/common/uct_model_settings.hpp"

#include <QColor>
#include <QByteArray>
#include <QImage>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QVector>

#include <atomic>
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
// Recognition, validation and rendering never touch the D3D presentation
// stream; they only consume CPU copies stamped at readback-request time.
namespace okuflow::uct {

// Immutable provenance of one CPU copy of presented pixels. Copies, jobs,
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

    bool SameScene(const SourceStamp& other) const noexcept
    {
        return sourceEpoch == other.sourceEpoch &&
               geometryGeneration == other.geometryGeneration;
    }
};

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

// Low-resolution luma copy used for residual validation and scene stability.
// The image can be any size; mapping uses the stamp's normalized transform.
struct ProbeFrame {
    SourceStamp stamp;
    QImage luma;                // Format_Grayscale8
    QVector<quint32> integral;  // summed-area table, (w + 1) x (h + 1)
    bool IsValid() const noexcept
    {
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
    QVector<float> values; // zero-mean, unit-variance samples
    bool valid{};
};

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
};

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

struct RecognizedWord {
    QString text;
    QRectF boundsPx; // pixels of the job image
    bool paddedBounds{}; // detector/CTC region needs worker-side ink fitting
    QRectF inkSearchPx; // optional source search, bounded by the line/neighbours
    TokenDisposition disposition{TokenDisposition::kCandidate};
    double confidence{1.0}; // backend evidence; never increased during admission
    QRectF protectedBoundsPx; // original detector span, before optional ink fitting
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
    bool backendPreparing{}; // use a temporary fallback, retain this candidate
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

struct LineColors {
    QColor foreground{Qt::black};
    QColor background{Qt::white};
};

// A recognized line already mapped to scene coordinates with its residual
// reference, prepared on the recognition worker so admission on the UI
// thread does no pixel work.
struct PreparedWord {
    QString text;
    QRectF sceneRect;
    TokenDisposition disposition{TokenDisposition::kCandidate};
    double confidence{1.0};
    QRectF protectedSceneRect;
};

struct PreparedLine {
    QVector<PreparedWord> words; // every known span, including rejected ones
    QRectF sceneRect;
    ReferencePatch reference;    // invalid when untrackable
};

struct RecognitionResult {
    std::uint64_t jobId{};
    std::uint64_t authority{};
    SourceStamp stamp;
    RecognitionOutput output;
    // Luma reference built from the job's own pixels (same stamp), so
    // acceptance validates against the exact recognized content.
    ProbeFrame reference;
    QVector<LineColors> lineColors; // parallel to output.lines
    // Parallel to output.lines when filled by the scheduler; Admit prepares
    // lines itself when this is empty (hand-built fixtures).
    QVector<PreparedLine> prepared;
    double durationMs{};
    bool deviceInput{};
    QByteArray sourcePixelDigest; // worker SHA-256 of exact owned recognition pixels
};

// Maps recognized words into scene coordinates and builds per-line residual
// references from `result.reference`. Pure; safe on any thread.
QVector<PreparedLine> PrepareLines(const RecognitionResult& result, double minimumWordHeightPx);

struct SchedulerStats {
    std::uint64_t submitted{};
    std::uint64_t coalesced{}; // pending jobs replaced before starting
    std::uint64_t started{};
    std::uint64_t completed{};
    std::uint64_t revoked{};   // finished after their authority was cancelled
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
                         bool probeOnly = false);
    ~RecognitionScheduler();
    RecognitionScheduler(const RecognitionScheduler&) = delete;
    RecognitionScheduler& operator=(const RecognitionScheduler&) = delete;

    // Takes an independently owned copy (detached by the caller or here).
    // Returns the job id, or 0 when the backend is unavailable/failed.
    std::uint64_t Submit(const SourceStamp& stamp, QImage pixels);
    // Zero-copy hand-off of already shared, immutable pixels.
    std::uint64_t Submit(const SourceStamp& stamp, SharedPixels pixels);
    std::uint64_t SubmitDevice(const SourceStamp& stamp, SharedDeviceImage device);
    void Cancel();
    std::uint64_t Authority() const;
    bool IsCurrent(const RecognitionResult& result) const;
    bool HasWork() const; // pending or in flight
    bool InFlight() const;
    BackendStatus Status() const;
    SchedulerStats Stats() const;

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

struct MatchTuning {
    double minimumScore{0.82};
    double minimumVisibleFraction{0.85};
    int searchRadiusPx{6}; // probe pixels
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
// unlimited) or `cancelled` is set. Unchecked lines are reported, never
// silently treated as valid.
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
// in-flight check loop at the next line. Work is CPU-only and budgeted, so
// destruction joins.
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
};

struct SceneWord {
    QString text;
    QRectF sceneRect; // validated offset applied
    LineColors colors;
    std::uint64_t lineId{};
    QVector<QRectF> protectedSceneRects; // all current protected spans, offset applied
    int wordIndex{-1}; // index into the owning AcceptedLine::words (accessibility key)
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
    // newer pixels than the probe keep that newer state.
    ValidationSummary ApplyChecks(const QVector<LineCheck>& checks, const SourceStamp& probeStamp);
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
                           bool advancingMedia = false);

    struct PendingWord {
        QString text;
        QRectF sceneRect;
        std::uint64_t sourceEpoch{};
        std::uint64_t geometryGeneration{};
    };

    ModelTuning tuning_;
    QVector<AcceptedLine> lines_;
    QVector<PendingWord> pending_;
    // pending_ is replaced as one batch after every successful admission;
    // one fixed-size evidence ticket covers that batch, rather than storing
    // hashes per token or retaining an unbounded history of source images.
    SourceStamp pendingEvidenceStamp_;
    QByteArray pendingEvidenceDigest_;
    std::uint64_t pendingEvidenceAuthority_{};
    std::uint64_t nextLineId_{1};
    bool immutable_{};
};

} // namespace okuflow::uct

#pragma once

#include "okuflow/common/view_transform.hpp"

#include <QColor>
#include <QImage>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QVector>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

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

struct RecognizedWord {
    QString text;
    QRectF boundsPx; // pixels of the job image
};

struct RecognizedLine {
    QVector<RecognizedWord> words;
};

struct RecognitionOutput {
    bool ok{};
    bool cancelled{};
    QString error;
    QVector<RecognizedLine> lines;
};

// Runs only on the scheduler's worker thread. Implementations must honor
// `cancelled` promptly where the platform allows, but may finish the native
// call; the scheduler keeps the input image alive until Recognize returns.
class Recognizer {
public:
    virtual ~Recognizer() = default;
    virtual BackendStatus Initialize() = 0;
    virtual RecognitionOutput Recognize(const QImage& image,
                                        const std::atomic<bool>& cancelled) = 0;
};

// Windows.Media.Ocr adapter (built-in Windows 10/11 OCR, no model files).
// Unavailable on other platforms or when no OCR-capable language exists.
std::unique_ptr<Recognizer> CreateDefaultRecognizer();

// --- Scheduler -------------------------------------------------------------

struct LineColors {
    QColor foreground{Qt::black};
    QColor background{Qt::white};
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
    double durationMs{};
};

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
                         int referenceDecimation = 2);
    ~RecognitionScheduler();
    RecognitionScheduler(const RecognitionScheduler&) = delete;
    RecognitionScheduler& operator=(const RecognitionScheduler&) = delete;

    // Takes an independently owned copy (detached by the caller or here).
    // Returns the job id, or 0 when the backend is unavailable/failed.
    std::uint64_t Submit(const SourceStamp& stamp, QImage pixels);
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

struct ReferencePatch {
    QRectF sceneRect; // sampled area (line box plus margin), scene units
    int columns{};
    int rows{};
    QVector<float> values; // zero-mean, unit-variance samples
    bool valid{};
};

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

PatchMatch MatchPatch(const ReferencePatch& patch, const ProbeFrame& probe,
                      const MatchTuning& tuning = {});

// Scene-registered similarity of two probes over their common area. Returns
// a correlation in [-1, 1], or nullopt when they share too little area.
std::optional<double> SceneSimilarity(const ProbeFrame& a, const ProbeFrame& b);

// True for tokens whose characters are commonly confused (0/O, 1/l/I, 5/S,
// 8/B, 2/Z) when letters and digits mix in one word, or which contain
// notation the MVP never claims (superscripts, math operators beyond simple
// arithmetic). Such words fail closed to original pixels.
bool IsHighRiskToken(const QString& text);

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
};

struct AcceptedLine {
    std::uint64_t id{};
    QRectF sceneRect;
    QVector<AcceptedWord> words;   // confirmed words only
    int omittedWords{};            // unconfirmed / high-risk words left original
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
};

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
    MatchTuning match;
};

struct ValidationSummary {
    int checked{};
    int valid{};
    int invalid{};
    int notVisible{};
};

class OverlayModel {
public:
    explicit OverlayModel(ModelTuning tuning = {});

    void Reset();
    AdmitVerdict Admit(const RecognitionResult& result,
                       const SourceStamp& current,
                       const ProbeFrame* latestProbe,
                       std::int64_t nowMs);
    // Revalidates every accepted line against a newer probe of the same scene.
    ValidationSummary Validate(const ProbeFrame& probe, std::int64_t nowMs);
    // Words safe to draw now, in scene units. A line is shown only if its
    // latest validation is fresh, or no newer camera frame has been
    // presented since it.
    QVector<SceneWord> SceneWords(std::int64_t nowMs, std::uint64_t currentFrameSeq) const;
    // SceneWords projected through a transform onto a pixel surface.
    QVector<DisplayWord> DisplayWords(const ViewTransform& transform, double width,
                                      double height, std::int64_t nowMs,
                                      std::uint64_t currentFrameSeq) const;
    // Plain reading-order text of accepted lines; omitted words become "…".
    QString ReadingText() const;
    const QVector<AcceptedLine>& Lines() const noexcept { return lines_; }
    int AcceptedWordCount() const;
    int PendingWordCount() const noexcept { return pending_.size(); }
    bool HasAcceptedText() const;

private:
    struct PendingWord {
        QString text;
        QRectF sceneRect;
        std::uint64_t sourceEpoch{};
        std::uint64_t geometryGeneration{};
    };

    ModelTuning tuning_;
    QVector<AcceptedLine> lines_;
    QVector<PendingWord> pending_;
    std::uint64_t nextLineId_{1};
};

} // namespace okuflow::uct

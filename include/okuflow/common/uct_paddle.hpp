#pragma once

#include "okuflow/common/ultra_clear_text.hpp"
#include <QByteArray>
#include <QDir>
#include <QStringList>
#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <span>

namespace okuflow { class AsyncCleanup; }

namespace okuflow::uct
{

// Pinned model input/output contract, shared by CPU and CUDA implementations.
struct PaddleAssets
{
    QString root;
    QString detector;
    QString recognizer;
    QByteArray detectorSha256, recognizerSha256;
    QStringList dictionary; // blank at 0, space appended exactly once
    QString error;
    bool IsValid() const
    {
        return error.isEmpty() && !dictionary.isEmpty();
    }
};
PaddleAssets LoadPaddleAssets(const QString &root, ModelTier detector = ModelTier::Tiny,
                             ModelTier recognizer = ModelTier::Small);
QString DefaultPaddleRoot();

struct PaddleQuad
{
    std::array<QPointF, 4> points; // TL, TR, BR, BL in source pixels
    float score{};
    int internalEdges{}; // recognition-window left/right cut (1/2), not source edges
};
struct PaddleTensor
{
    std::vector<float> data;
    std::vector<std::int64_t> shape;
};
struct PaddleToken
{
    int id{}, first{}, last{};
    float confidence{};
};
RecognizedLine PaddleDecodeTokens(std::span<const PaddleToken> tokens, int steps,
                                  const PaddleQuad &quad, const QStringList &dictionary);
// Optional off-UI diagnostic receipt for an exact TensorRT State closure.
// Completed means that closure was released after known stream completion;
// nested AnalysisPool storage cleanup and vendor threads have separate lifetimes.
// An observer exception is ignored and never changes cleanup/quarantine.
using TensorRtRetirementObserver = std::function<void(std::shared_ptr<AsyncCleanup>)>;
// Explicit diagnostic only: retain at most four immutable owned inputs and the
// compact detector windows copied by ordinary recognition. Later calls on the
// SAME leased pixels replay those windows, isolating recognition from detection.
// Capacity/allocation refusal leaves ordinary recognition authoritative.
// Worker-confined, including queries; never installed by the automatic backend.
class TensorRtCropDiagnostic {
public:
    struct Crop { std::array<float, 8> xy{}; float score{}; int validWidth{}, internalEdges{}; };
    struct Region { QRect bounds; std::vector<Crop> crops; };
    struct Plan { SharedDeviceImage source; QByteArray detectorSha256; std::vector<Region> regions; };
    int CapturedSources() const { return int(plans_.size()); }
    std::uint64_t ReplayCalls() const { return replayCalls_; }
private:
    friend struct TensorRtCropDiagnosticAccess;
    friend struct TensorRtCropDiagnosticTestAccess;
    static constexpr std::size_t kMaxSources = 4;
    // Diagnostic refusal must not invalidate completed OCR or healthy engines.
    bool TryCommit(Plan&& plan) noexcept;
    std::vector<Plan> plans_;
    std::uint64_t replayCalls_{};
};
std::unique_ptr<Recognizer> CreateTensorRtRecognizer(const QString &root = {},
                                                     const QString &sdk = {},
                                                     ModelTier detector = ModelTier::Medium,
                                                     ModelTier recognizer = ModelTier::Medium,
                                                     TensorRtRetirementObserver retirementObserver = {},
                                                     std::shared_ptr<TensorRtCropDiagnostic> cropDiagnostic = {});

// All calls are worker-confined; the optional runtime is loaded by absolute path.
class PaddleCpuSession
{
  public:
    virtual ~PaddleCpuSession() = default;
    virtual PaddleTensor Run(std::span<const float> data, std::span<const std::int64_t> shape) = 0;
};
std::unique_ptr<PaddleCpuSession> OpenPaddleCpuSession(const QString &runtime,
                                                       const QString &model);

// CPU reference algorithms. GPU implementations retain the same channel order,
// resize, threshold, rectification, dictionary and CTC conventions.
QSize PaddleDetectionSize(QSize source);
// Whole-scene context followed by bounded overlapping detail crops. Each
// detector invocation retains the existing <=960-pixel engine contract.
QVector<QRect> PaddleDetectionRegions(QSize source);
QVector<PaddleQuad> PaddleRecognitionWindows(const PaddleQuad& line);
// Maps crop-local evidence back to the complete scene, excludes internal
// crop-edge fragments and reconciles overlapping words without duplicates.
void PaddleMergeRecognition(QVector<RecognizedLine>& output, RecognizedLine line,
                             QRect region, QSize source);
std::vector<float> PaddleNormalize(const QImage &image, QSize target, bool detection);
QVector<PaddleQuad> PaddleDetect(std::span<const float> map, int width, int height, QSize source,
                                 const std::atomic<bool> &cancelled);
std::vector<float> PaddleRectify(const QImage &image, const PaddleQuad &quad, int width,
                                 int validWidth);
RecognizedLine PaddleDecode(std::span<const float> probabilities, int steps, int classes,
                            const PaddleQuad &quad, const QStringList &dictionary);
std::unique_ptr<Recognizer> CreatePaddleCpuRecognizer(const QString &root = {},
                                                    ModelTier detector = ModelTier::Tiny,
                                                    ModelTier recognizer = ModelTier::Small);
std::unique_ptr<Recognizer> CreateWindowsRecognizer();

// Chooses TensorRT, Paddle CPU, then Windows OCR. A failed backend is retired
// for this recognizer's lifetime; cancellation never triggers fallback/retry.
// An injected bridge serves complete jobs while a preferred backend prepares;
// default production uses Windows OCR, deferring costly Paddle CPU startup.
// Bridge results retain backendPreparing even when ok, permitting bounded
// preferred-backend refinement without changing normal evidence admission.
std::unique_ptr<Recognizer> CreateAutomaticRecognizer(const ModelSettings& models = {});
std::unique_ptr<Recognizer> CreateAutomaticRecognizer(
    std::vector<std::unique_ptr<Recognizer>> candidates,
    std::optional<std::size_t> preparationBridge = {});

} // namespace okuflow::uct

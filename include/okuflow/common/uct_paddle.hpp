#pragma once

#include "okuflow/common/ultra_clear_text.hpp"
#include <QByteArray>
#include <QDir>
#include <QStringList>
#include <array>
#include <span>

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
std::unique_ptr<Recognizer> CreateTensorRtRecognizer(const QString &root = {},
                                                     const QString &sdk = {},
                                                     ModelTier detector = ModelTier::Medium,
                                                     ModelTier recognizer = ModelTier::Medium);

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
std::unique_ptr<Recognizer> CreateAutomaticRecognizer(const ModelSettings& models = {});
std::unique_ptr<Recognizer> CreateAutomaticRecognizer(
    std::vector<std::unique_ptr<Recognizer>> candidates);

} // namespace okuflow::uct

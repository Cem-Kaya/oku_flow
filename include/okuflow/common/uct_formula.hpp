#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>
#include <QColor>
#include <QRect>
#include <QImage>
#include "okuflow/common/uct_formula_settings.hpp"
#include "okuflow/common/uct_device_image.hpp"
#include <atomic>
#include <chrono>
#include <memory>
#include <span>

namespace okuflow::uct::formula {
QString ModelName(Model model);
struct Result {
    bool ok{}, cancelled{};
    QString code, detail, latex;
    QVector<qint64> tokens;
    int iterations{};
    double milliseconds{};
    bool compatible{}; // confirmed tensor execution, independent of recognized text
    QRect boundsPx;
    QColor foreground,background;
};
// Worker-confined, separate from ordinary OCR and its fallback authority.
// Input is a preprocessed FLOAT 1x1x768x768 tensor. This interface does not
// authorize reading presenter pixels, drawing text, or changing app settings.
class Engine {
public:
    virtual ~Engine() = default;
    virtual Result Recognize(std::span<const float> tensor, const std::atomic<bool>& cancelled,
                             std::chrono::steady_clock::time_point deadline, int tokenLimit = 0) = 0;
    virtual Result RecognizeImage(const QImage&,const QRect&,const std::atomic<bool>&,
                                 std::chrono::steady_clock::time_point);
    virtual Result RecognizeDevice(SharedDeviceImage,const QRect&,const std::atomic<bool>&,
                                  std::chrono::steady_clock::time_point);
};
struct Prepared {
    std::unique_ptr<Engine> engine;
    QString code, detail;
    bool cancelled{};
};
// Blocking worker-only preparation from exact compiled partition/config pins.
// Native TensorRT TF32, with no Python, subprocess, CPU inference or BF16 fallback.
// Sources are read-only; only complete identity-bound engines enter cacheRoot.
Prepared Prepare(Model model, const QString& sourceRoot, const QString& cacheRoot,
                 const std::atomic<bool>& cancelled,
                 std::chrono::steady_clock::time_point deadline);
} // namespace okuflow::uct::formula

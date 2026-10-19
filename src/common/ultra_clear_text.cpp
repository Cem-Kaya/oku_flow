#include "okuflow/common/ultra_clear_text.hpp"
#include <QCryptographicHash>

#include <QElapsedTimer>
#include <QHash>
#include <QSet>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>

namespace okuflow::uct {
namespace {

bool UsableTransform(const ViewTransform& t, double width, double height)
{
    return t.valid && width > 0.0 && height > 0.0 && t.sourceWidth > 0.0f &&
           t.sourceHeight > 0.0f && t.destinationWidth > 0.0f && t.destinationHeight > 0.0f;
}

int Luma(const uchar* bgra)
{
    return (29 * bgra[0] + 150 * bgra[1] + 77 * bgra[2]) >> 8;
}

// Summed-area value at a fractional lattice position (bilinear in the table).
double IntegralAt(const ProbeFrame& probe, double x, double y)
{
    const int w = probe.luma.width();
    const int h = probe.luma.height();
    x = std::clamp(x, 0.0, static_cast<double>(w));
    y = std::clamp(y, 0.0, static_cast<double>(h));
    const int x0 = std::min(static_cast<int>(x), w);
    const int y0 = std::min(static_cast<int>(y), h);
    const int x1 = std::min(x0 + 1, w);
    const int y1 = std::min(y0 + 1, h);
    const double ax = x - x0;
    const double ay = y - y0;
    const int stride = w + 1;
    const quint32* s = probe.integral.constData();
    const double top = s[y0 * stride + x0] + (static_cast<double>(s[y0 * stride + x1]) - s[y0 * stride + x0]) * ax;
    const double bottom = s[y1 * stride + x0] + (static_cast<double>(s[y1 * stride + x1]) - s[y1 * stride + x0]) * ax;
    return top + (bottom - top) * ay;
}

// Box-filtered luma over a probe-pixel rectangle centred at (cx, cy). Cells
// cover the same scene area in every probe, so values stay comparable when
// the zoom differs between the recognized copy and a later probe.
bool BoxAverage(const ProbeFrame& probe, double cx, double cy, double halfW, double halfH, float& value)
{
    halfW = std::max(halfW, 0.5);
    halfH = std::max(halfH, 0.5);
    const double x0 = cx - halfW;
    const double x1 = cx + halfW;
    const double y0 = cy - halfH;
    const double y1 = cy + halfH;
    // Roundoff at a shared scene edge is not missing image content. IntegralAt
    // clamps this sub-millionth-pixel allowance on both CPU and GPU.
    constexpr double edge = 1e-7;
    if (x0 < -edge || y0 < -edge || x1 > probe.luma.width() + edge || y1 > probe.luma.height() + edge) {
        return false;
    }
    const double sum = IntegralAt(probe, x1, y1) - IntegralAt(probe, x0, y1) -
                       IntegralAt(probe, x1, y0) + IntegralAt(probe, x0, y0);
    value = static_cast<float>(sum / ((x1 - x0) * (y1 - y0)));
    return true;
}

double PixelsPerSceneX(const ProbeFrame& probe)
{
    const ViewTransform& t = probe.stamp.viewTransform;
    return static_cast<double>(t.destinationWidth) * probe.luma.width() / t.sourceWidth;
}

double PixelsPerSceneY(const ProbeFrame& probe)
{
    const ViewTransform& t = probe.stamp.viewTransform;
    return static_cast<double>(t.destinationHeight) * probe.luma.height() / t.sourceHeight;
}

struct PatchGeometry {
    QVector<QPointF> probePixels; // cell centres in probe pixels
    double halfWidth{};           // cell half extent in probe pixels
    double halfHeight{};
    double sceneUnitsPerPixelX{};
    double sceneUnitsPerPixelY{};
};

PatchGeometry ProjectPatch(const ReferencePatch& patch, const ProbeFrame& probe)
{
    PatchGeometry g;
    const double w = probe.luma.width();
    const double h = probe.luma.height();
    const ViewTransform& t = probe.stamp.viewTransform;
    g.probePixels.reserve(patch.columns * patch.rows);
    for (int r = 0; r < patch.rows; ++r) {
        for (int c = 0; c < patch.columns; ++c) {
            const QPointF scene(
                patch.sceneRect.x() + (c + 0.5) / patch.columns * patch.sceneRect.width(),
                patch.sceneRect.y() + (r + 0.5) / patch.rows * patch.sceneRect.height());
            g.probePixels.push_back(SceneToViewPixel(t, w, h, scene));
        }
    }
    g.halfWidth = 0.5 * patch.sceneRect.width() / patch.columns * PixelsPerSceneX(probe);
    g.halfHeight = 0.5 * patch.sceneRect.height() / patch.rows * PixelsPerSceneY(probe);
    g.sceneUnitsPerPixelX = 1.0 / PixelsPerSceneX(probe);
    g.sceneUnitsPerPixelY = 1.0 / PixelsPerSceneY(probe);
    return g;
}

struct Correlation {
    double score{-1.0};
    double visibleFraction{};
};

Correlation Correlate(const ReferencePatch& patch, const PatchGeometry& g, const ProbeFrame& probe,
                      double dx, double dy, int stride)
{
    Correlation out;
    const int n = patch.values.size();
    if (n == 0 || stride <= 0) {
        return out;
    }
    double sumA = 0.0, sumB = 0.0, sumAA = 0.0, sumBB = 0.0, sumAB = 0.0;
    int considered = 0;
    int visible = 0;
    for (int i = 0; i < n; i += stride) {
        ++considered;
        float v = 0.0f;
        const QPointF& p = g.probePixels[i];
        if (!BoxAverage(probe, p.x() + dx, p.y() + dy, g.halfWidth, g.halfHeight, v)) {
            continue;
        }
        ++visible;
        const double a = patch.values[i];
        sumA += a;
        sumB += v;
        sumAA += a * a;
        sumBB += static_cast<double>(v) * v;
        sumAB += a * v;
    }
    out.visibleFraction = considered ? static_cast<double>(visible) / considered : 0.0;
    if (visible < 8) {
        return out;
    }
    const double count = visible;
    const double covariance = sumAB - sumA * sumB / count;
    const double varA = sumAA - sumA * sumA / count;
    const double varB = sumBB - sumB * sumB / count;
    // A flat probe region cannot confirm printed text.
    if (varA <= 1e-9 || varB <= count * 9.0) {
        out.score = 0.0;
        return out;
    }
    out.score = covariance / std::sqrt(varA * varB);
    return out;
}

double Iou(const QRectF& a, const QRectF& b)
{
    const QRectF i = a.intersected(b);
    if (i.isEmpty()) {
        return 0.0;
    }
    const double inter = i.width() * i.height();
    const double uni = a.width() * a.height() + b.width() * b.height() - inter;
    return uni > 0.0 ? inter / uni : 0.0;
}

LineColors MeasureLineColors(const QImage& image, const QRect& bounds)
{
    LineColors colors;
    const QRect box = bounds.intersected(image.rect());
    if (box.isEmpty()) {
        return colors;
    }
    long long lumaSum = 0;
    int count = 0;
    for (int y = box.top(); y <= box.bottom(); y += 2) {
        const uchar* row = image.constScanLine(y);
        for (int x = box.left(); x <= box.right(); x += 2) {
            lumaSum += Luma(row + x * 4);
            ++count;
        }
    }
    if (count == 0) {
        return colors;
    }
    const int threshold = static_cast<int>(lumaSum / count);
    long long dark[3]{}, light[3]{};
    int darkCount = 0, lightCount = 0;
    for (int y = box.top(); y <= box.bottom(); y += 2) {
        const uchar* row = image.constScanLine(y);
        for (int x = box.left(); x <= box.right(); x += 2) {
            const uchar* px = row + x * 4;
            const bool isDark = Luma(px) < threshold;
            long long* target = isDark ? dark : light;
            ++(isDark ? darkCount : lightCount);
            target[0] += px[2];
            target[1] += px[1];
            target[2] += px[0];
        }
    }
    const auto average = [](const long long* sum, int n) {
        return n ? QColor(static_cast<int>(sum[0] / n), static_cast<int>(sum[1] / n),
                          static_cast<int>(sum[2] / n))
                 : QColor();
    };
    const QColor darkColor = average(dark, darkCount);
    const QColor lightColor = average(light, lightCount);
    // Dense/bold print can cover most of a tight OCR box. Prefer surrounding
    // paper/board evidence to the minority-ink heuristic when a margin exists.
    // This is bounded local color work on the recognition worker, never paint.
    const int margin = std::clamp(box.height() / 6, 2, 12);
    const QRect surround = box.adjusted(-margin, -margin, margin, margin).intersected(image.rect());
    int darkSurround = 0, lightSurround = 0;
    int surroundHistogram[2][3][256]{};
    for (int y = surround.top(); y <= surround.bottom(); y += 2) {
        const uchar* row = image.constScanLine(y);
        for (int x = surround.left(); x <= surround.right(); x += 2) {
            if (box.contains(x, y)) continue;
            const uchar* px = row + x * 4;
            const int side = Luma(px) < threshold ? 0 : 1;
            ++(side == 0 ? darkSurround : lightSurround);
            for (int channel = 0; channel < 3; ++channel)
                ++surroundHistogram[side][channel][px[2-channel]];
        }
    }
    const bool darkText = darkSurround != lightSurround
                              ? lightSurround > darkSurround
                              : darkCount <= lightCount;
    colors.foreground = darkText ? darkColor : lightColor;
    colors.background = darkText ? lightColor : darkColor;
    const int backgroundSide = darkText ? 1 : 0;
    const int surroundingSamples = darkText ? lightSurround : darkSurround;
    if (surroundingSamples >= 8) {
        int median[3]{};
        for (int channel = 0; channel < 3; ++channel) {
            int total = 0;
            for (int value = 0; value < 256; ++value) {
                total += surroundHistogram[backgroundSide][channel][value];
                if (total > surroundingSamples/2) { median[channel]=value;break; }
            }
        }
        colors.background = QColor(median[0],median[1],median[2]);
    }
    if (!colors.foreground.isValid() || !colors.background.isValid()) {
        colors = {};
    }
    return colors;
}

LineColors StableLineColors(LineColors measured, const LineColors *previous = nullptr) {
    // A fixed 64-level brightness grid, not a per-frame fitted palette. Scaling
    // RGB together retains hue; the camera image itself is never posterized.
    const int value = std::max(
        {measured.background.red(), measured.background.green(), measured.background.blue()});
    if (value > 0) {
        const int snapped =
            static_cast<int>(std::lround(std::round(value * 63.0 / 255.0) * 255.0 / 63.0));
        measured.background = QColor(measured.background.red() * snapped / value,
                                     measured.background.green() * snapped / value,
                                     measured.background.blue() * snapped / value);
    }
    if (previous) {
        const auto difference = [](const QColor &a, const QColor &b) {
            return std::max({std::abs(a.red() - b.red()), std::abs(a.green() - b.green()),
                             std::abs(a.blue() - b.blue())});
        };
        const auto polarity = [](const LineColors &pair) {
            return qGray(pair.foreground.rgb()) < qGray(pair.background.rgb());
        };
        if (polarity(measured) == polarity(*previous) &&
            difference(measured.foreground, previous->foreground) <= 8 &&
            difference(measured.background, previous->background) <= 8)
            return *previous;
    }
    return measured;
}

QRectF RefineWordInkBounds(const QImage &image, const RecognizedWord &word,
                           const LineColors &colors) {
    const QRectF detected = word.boundsPx;
    const QRect box = (word.inkSearchPx.isEmpty() ? detected : word.inkSearchPx)
                          .toAlignedRect()
                          .intersected(image.rect());
    if (box.isEmpty())
        return detected;
    const auto luma = [](const QColor &color) {
        return (77 * color.red() + 150 * color.green() + 29 * color.blue()) / 256;
    };
    const int ink = luma(colors.foreground), paper = luma(colors.background);
    if (std::abs(ink - paper) < 24)
        return detected;
    const int threshold = (ink + paper) / 2;
    // Detector unclip and CTC spans include background padding. Fit to the
    // visible source ink, not that padded rectangle, after inference completes.
    // Bound work for unusually large boxes; all scanning stays on the worker.
    const int step = std::max(1, static_cast<int>(std::ceil(std::sqrt(
                                     static_cast<double>(box.width()) * box.height() / 16384.0))));
    QVector<QPoint> samples;
    QVector<int> rows(box.height(), 0);
    for (int y = box.top(); y <= box.bottom(); y += step) {
        const uchar *row = image.constScanLine(y);
        for (int x = box.left(); x <= box.right(); x += step) {
            const int value = Luma(row + x * 4);
            if (ink < paper ? value >= threshold : value <= threshold)
                continue;
            samples.push_back(QPoint(x, y));
            ++rows[y - box.top()];
        }
    }
    if (samples.size() < 8)
        return detected;
    struct InkRun {
        int top;
        int bottom;
        int mass;
    };
    QVector<InkRun> runs;
    for (int y = 0; y < rows.size(); ++y) {
        if (!rows[y])
            continue;
        if (runs.isEmpty() || y - runs.last().bottom > step * 2)
            runs.push_back({y, y, rows[y]});
        else {
            runs.last().bottom = y;
            runs.last().mass += rows[y];
        }
    }
    int main = 0;
    for (int i = 1; i < runs.size(); ++i)
        if (runs[i].mass > runs[main].mass)
            main = i;
    // Separated substantial bands may be a multi-part glyph; leave ambiguous
    // layouts intact instead of mistaking one stroke for the entire word.
    if (runs[main].mass < samples.size() * .55)
        return detected;
    int top = runs[main].top + box.top(), bottom = runs[main].bottom + box.top();
    bool detachedMarks = word.text.contains(QLatin1Char('i'), Qt::CaseInsensitive) ||
                         word.text.contains(QLatin1Char('j'), Qt::CaseInsensitive);
    for (QChar c : word.text.normalized(QString::NormalizationForm_D))
        detachedMarks = detachedMarks || c.isMark() || QStringLiteral("!?:;").contains(c);
    if (detachedMarks)
        for (int i = 0; i < runs.size(); ++i) {
            if (i == main || runs[i].mass > runs[main].mass * .3)
                continue;
            const int a = runs[i].top + box.top(), b = runs[i].bottom + box.top();
            const int gap = std::max({top - b, a - bottom, 0});
            if (gap <= std::max(2.0, detected.height() * .22)) {
                top = std::min(top, a);
                bottom = std::max(bottom, b);
            }
        }
    int left = box.right() + 1, right = box.left() - 1;
    for (const QPoint &sample : samples)
        if (sample.y() >= top && sample.y() <= bottom) {
            left = std::min(left, sample.x());
            right = std::max(right, sample.x());
        }
    const QRectF refined(QPointF(left, top), QPointF(std::min(box.right() + 1, right + step),
                                                     std::min(box.bottom() + 1, bottom + step)));
    // Tiny/ambiguous isolated marks are not evidence for shrinking a word.
    if (refined.width() < detected.width() * 0.35 || refined.height() < detected.height() * 0.35)
        return detected;
    return refined;
}

QImage ToOwnedArgb(const std::uint8_t* bgra, int width, int height, int stride)
{
    if (!bgra || width <= 0 || height <= 0 || stride < width * 4) {
        return {};
    }
    QImage owned(width, height, QImage::Format_ARGB32);
    if (owned.isNull()) {
        return {};
    }
    for (int y = 0; y < height; ++y) {
        std::copy_n(bgra + static_cast<std::size_t>(y) * stride, static_cast<std::size_t>(width) * 4,
                    owned.scanLine(y));
    }
    return owned;
}

QByteArray SourcePixelDigest(const QImage& pixels)
{
    if (pixels.isNull()) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    // Allocation padding is not source content and cannot establish distinct
    // evidence for two otherwise identical images.
    for (int y = 0; y < pixels.height(); ++y)
        hash.addData(QByteArrayView(reinterpret_cast<const char*>(pixels.constScanLine(y)),
                                    static_cast<qsizetype>(pixels.width()) * 4));
    return hash.result();
}

} // namespace

// --- Pixel hand-off -----------------------------------------------------------

std::shared_ptr<PixelPool> PixelPool::Create(int capacity)
{
    return std::shared_ptr<PixelPool>(new PixelPool(std::max(1, capacity)));
}

std::vector<std::uint8_t> PixelPool::Take(std::size_t minimumSize)
{
    std::lock_guard lock(mutex_);
    for (auto it = free_.begin(); it != free_.end(); ++it) {
        if (it->size() >= minimumSize) {
            std::vector<std::uint8_t> bytes = std::move(*it);
            free_.erase(it);
            return bytes;
        }
    }
    return {};
}

void PixelPool::Give(std::vector<std::uint8_t>&& bytes)
{
    std::lock_guard lock(mutex_);
    if (static_cast<int>(free_.size()) < capacity_ && !bytes.empty()) {
        free_.push_back(std::move(bytes));
    }
}

SharedPixels PixelPool::Wrap(PixelBuffer* buffer)
{
    std::weak_ptr<PixelPool> weak = weak_from_this();
    return SharedPixels(buffer, [weak](const PixelBuffer* owned) {
        auto* mutableBuffer = const_cast<PixelBuffer*>(owned);
        if (const std::shared_ptr<PixelPool> pool = weak.lock()) {
            pool->Give(std::move(mutableBuffer->bytes));
        }
        delete mutableBuffer;
    });
}

SharedPixels PixelPool::Adopt(std::vector<std::uint8_t>& bytes, int width, int height, int strideBytes)
{
    const std::size_t needed = static_cast<std::size_t>(std::max(0, strideBytes)) *
                               static_cast<std::size_t>(std::max(0, height));
    if (width <= 0 || height <= 0 || strideBytes < width * 4 || bytes.size() < needed) {
        return {};
    }
    auto* buffer = new PixelBuffer;
    buffer->bytes.swap(bytes);
    buffer->width = width;
    buffer->height = height;
    buffer->strideBytes = strideBytes;
    // A recycled vector of the same size makes the presenter's next resize()
    // a no-op; otherwise it allocates once, as before.
    bytes = Take(needed);
    if (bytes.empty()) {
        std::lock_guard lock(mutex_);
        ++allocations_; // the caller's next fill allocates once
    }
    if (bytes.size() > needed) {
        bytes.resize(needed);
    }
    return Wrap(buffer);
}

SharedPixels PixelPool::Copy(const std::uint8_t* bgra, int width, int height, int strideBytes)
{
    if (!bgra || width <= 0 || height <= 0 || strideBytes < width * 4) {
        return {};
    }
    const std::size_t rowBytes = static_cast<std::size_t>(width) * 4;
    const std::size_t needed = rowBytes * static_cast<std::size_t>(height);
    auto* buffer = new PixelBuffer;
    buffer->bytes = Take(needed);
    if (buffer->bytes.size() < needed) {
        std::lock_guard lock(mutex_);
        ++allocations_;
    }
    buffer->bytes.resize(needed);
    for (int y = 0; y < height; ++y) {
        std::copy_n(bgra + static_cast<std::size_t>(y) * strideBytes, rowBytes,
                    buffer->bytes.data() + static_cast<std::size_t>(y) * rowBytes);
    }
    buffer->width = width;
    buffer->height = height;
    buffer->strideBytes = static_cast<int>(rowBytes);
    return Wrap(buffer);
}

int PixelPool::FreeCount() const
{
    std::lock_guard lock(mutex_);
    return static_cast<int>(free_.size());
}

std::uint64_t PixelPool::Allocations() const
{
    std::lock_guard lock(mutex_);
    return allocations_;
}

QImage WrapPixels(const SharedPixels& pixels)
{
    if (!pixels || !pixels->IsValid()) {
        return {};
    }
    // Read-only constructor: any non-const access deep-copies instead of
    // writing into the shared buffer.
    auto* keepAlive = new SharedPixels(pixels);
    return QImage(
        static_cast<const uchar*>(pixels->bytes.data()), pixels->width, pixels->height, pixels->strideBytes,
        QImage::Format_ARGB32, [](void* info) { delete static_cast<SharedPixels*>(info); }, keepAlive);
}

// --- Geometry ----------------------------------------------------------------

bool ViewPixelToScene(const ViewTransform& t, double width, double height,
                      const QPointF& viewPixel, QPointF& scenePoint)
{
    if (!UsableTransform(t, width, height)) {
        return false;
    }
    const double u = viewPixel.x() / width;
    const double v = viewPixel.y() / height;
    scenePoint = {t.sourceX + (u - t.destinationX) / t.destinationWidth * t.sourceWidth,
                  t.sourceY + (v - t.destinationY) / t.destinationHeight * t.sourceHeight};
    return true;
}

QPointF SceneToViewPixel(const ViewTransform& t, double width, double height,
                         const QPointF& scenePoint)
{
    if (!UsableTransform(t, width, height)) {
        return {};
    }
    const double u = t.destinationX + (scenePoint.x() - t.sourceX) / t.sourceWidth * t.destinationWidth;
    const double v = t.destinationY + (scenePoint.y() - t.sourceY) / t.sourceHeight * t.destinationHeight;
    return {u * width, v * height};
}

QRectF ViewRectToScene(const ViewTransform& t, double width, double height, const QRectF& viewRect)
{
    QPointF a, b;
    if (!ViewPixelToScene(t, width, height, viewRect.topLeft(), a) ||
        !ViewPixelToScene(t, width, height, viewRect.bottomRight(), b)) {
        return {};
    }
    return QRectF(a, b).normalized();
}

QRectF SceneRectToView(const ViewTransform& t, double width, double height, const QRectF& sceneRect)
{
    if (!UsableTransform(t, width, height)) {
        return {};
    }
    return QRectF(SceneToViewPixel(t, width, height, sceneRect.topLeft()),
                  SceneToViewPixel(t, width, height, sceneRect.bottomRight()))
        .normalized();
}

ProbeFrame MakeProbeFrame(const std::uint8_t* bgra, int width, int height, int strideBytes,
                          int decimation, const SourceStamp& stamp)
{
    ProbeFrame probe;
    if (!bgra || width <= 0 || height <= 0 || strideBytes < width * 4 || decimation < 1 ||
        !stamp.viewTransform.valid) {
        return probe;
    }
    const int ow = width / decimation;
    const int oh = height / decimation;
    if (ow < 8 || oh < 8) {
        return probe;
    }
    QImage luma(ow, oh, QImage::Format_Grayscale8);
    if (luma.isNull()) {
        return probe;
    }
    // At most 3x3 samples per block keep the cost proportional to the probe
    // size rather than the viewport resolution.
    const int step = std::max(1, decimation / 2);
    const int perAxis = (decimation + step - 1) / step;
    const int area = perAxis * perAxis;
    for (int y = 0; y < oh; ++y) {
        uchar* out = luma.scanLine(y);
        for (int x = 0; x < ow; ++x) {
            int sum = 0;
            for (int dy = 0; dy < decimation; dy += step) {
                const std::uint8_t* row = bgra + static_cast<std::size_t>(y * decimation + dy) * strideBytes;
                for (int dx = 0; dx < decimation; dx += step) {
                    sum += Luma(row + static_cast<std::size_t>(x * decimation + dx) * 4);
                }
            }
            out[x] = static_cast<uchar>(sum / area);
        }
    }
    // Summed-area table with a zero first row/column, (ow + 1) x (oh + 1).
    QVector<quint32> integral((ow + 1) * (oh + 1), 0u);
    for (int y = 0; y < oh; ++y) {
        const uchar* row = luma.constScanLine(y);
        quint32 rowSum = 0;
        for (int x = 0; x < ow; ++x) {
            rowSum += row[x];
            integral[(y + 1) * (ow + 1) + x + 1] = integral[y * (ow + 1) + x + 1] + rowSum;
        }
    }
    probe.stamp = stamp;
    probe.luma = std::move(luma);
    probe.integral = std::move(integral);
    return probe;
}

// --- Validation primitives ---------------------------------------------------

int ProbeFrame::Width() const noexcept { return resident ? resident->Width() : luma.width(); }
int ProbeFrame::Height() const noexcept { return resident ? resident->Height() : luma.height(); }

ReferencePatch BuildReferencePatch(const ProbeFrame& reference, const QRectF& lineRect)
{
    if (reference.resident) return reference.resident->Reference(reference.stamp, lineRect);
    ReferencePatch patch;
    if (!reference.IsValid() || lineRect.isEmpty()) {
        return patch;
    }
    const double marginY = lineRect.height() * 0.3;
    const double marginX = lineRect.height() * 0.15;
    // Margins never reach beyond the copy; the line itself must be inside.
    const double w = reference.luma.width();
    const double h = reference.luma.height();
    const QRectF visible = ViewRectToScene(reference.stamp.viewTransform, w, h,
                                           QRectF(1.0, 1.0, w - 2.0, h - 2.0));
    if (!visible.contains(lineRect)) {
        return patch;
    }
    patch.sceneRect = lineRect.adjusted(-marginX, -marginY, marginX, marginY).intersected(visible);
    const QRectF px = SceneRectToView(reference.stamp.viewTransform, reference.luma.width(),
                                      reference.luma.height(), patch.sceneRect);
    patch.columns = std::clamp(static_cast<int>(std::lround(px.width())), 8, 64);
    patch.rows = std::clamp(static_cast<int>(std::lround(px.height())), 4, 12);
    PatchGeometry g = ProjectPatch(patch, reference);
    patch.values.reserve(g.probePixels.size());
    double sum = 0.0, sumSq = 0.0;
    for (const QPointF& p : g.probePixels) {
        float v = 0.0f;
        if (!BoxAverage(reference, p.x(), p.y(), g.halfWidth, g.halfHeight, v)) {
            return {};
        }
        patch.values.push_back(v);
        sum += v;
        sumSq += static_cast<double>(v) * v;
    }
    const double n = patch.values.size();
    const double mean = sum / n;
    const double variance = std::max(0.0, sumSq / n - mean * mean);
    const double deviation = std::sqrt(variance);
    // Printed text gives strong local contrast; a flat area is untrackable.
    if (deviation < 6.0) {
        return {};
    }
    for (float& v : patch.values) {
        v = static_cast<float>((v - mean) / deviation);
    }
    patch.valid = true;
    return patch;
}

PatchMatch MatchPatch(const ReferencePatch& patch, const ProbeFrame& probe, const MatchTuning& tuning,
                      std::optional<QPointF> seedSceneOffset)
{
    PatchMatch match;
    // Resident matching is explicit worker work (CheckLines / corroboration).
    // Never make UI-thread admission wait for a GPU stream.
    if (probe.resident) return match;
    if (!patch.valid || !probe.IsValid()) {
        return match;
    }
    const PatchGeometry g = ProjectPatch(patch, probe);
    const Correlation zero = Correlate(patch, g, probe, 0.0, 0.0, 1);
    match.visible = zero.visibleFraction >= tuning.minimumVisibleFraction;
    if (!match.visible) {
        return match;
    }
    const double confident = tuning.minimumScore + 0.08;
    const int radius = std::clamp(tuning.searchRadiusPx, 1, 10);
    double bestScore = zero.score;
    double bestX = 0.0, bestY = 0.0;
    // Static fast path: an unmoved, unchanged line is the common case.
    if (zero.score < confident) {
        double searchX = 0.0, searchY = 0.0;
        bool seeded = false;
        if (seedSceneOffset) {
            // Motion continuity: try the last validated offset first, but only
            // inside the same bounded window the grid would search.
            const double sx = std::round(seedSceneOffset->x() / g.sceneUnitsPerPixelX);
            const double sy = std::round(seedSceneOffset->y() / g.sceneUnitsPerPixelY);
            if ((sx != 0.0 || sy != 0.0) && std::abs(sx) <= radius && std::abs(sy) <= radius) {
                const Correlation c = Correlate(patch, g, probe, sx, sy, 1);
                if (c.visibleFraction >= tuning.minimumVisibleFraction && c.score >= confident) {
                    bestScore = c.score;
                    bestX = sx;
                    bestY = sy;
                    searchX = sx;
                    searchY = sy;
                    seeded = true;
                }
            }
        }
        if (!seeded) {
            // Coarse scores rank only coarse candidates. They must never
            // replace the already measured full-density zero hypothesis.
            double coarseScore = -1.0;
            double coarseBestX = 0.0, coarseBestY = 0.0;
            for (int dy = -radius; dy <= radius; dy += 2) {
                for (int dx = -radius; dx <= radius; dx += 2) {
                    const Correlation c = Correlate(patch, g, probe, dx, dy, 3);
                    if (c.visibleFraction >= tuning.minimumVisibleFraction && c.score > coarseScore) {
                        coarseScore = c.score;
                        coarseBestX = dx;
                        coarseBestY = dy;
                    }
                }
            }
            // A sparse proposal may have a poor dense centre but a valid
            // odd-pixel neighbor. Keep its neighborhood independently from
            // the best fully evaluated hypothesis.
            searchX = coarseBestX;
            searchY = coarseBestY;
            const Correlation dense = Correlate(patch, g, probe, coarseBestX, coarseBestY, 1);
            if (dense.visibleFraction >= tuning.minimumVisibleFraction && dense.score > bestScore) {
                bestScore = dense.score;
                bestX = coarseBestX;
                bestY = coarseBestY;
            }
        }
        const double coarseX = searchX, coarseY = searchY;
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dy == 0) {
                    continue;
                }
                const double x = coarseX + dx;
                const double y = coarseY + dy;
                if (std::abs(x) > radius + 1 || std::abs(y) > radius + 1) {
                    continue;
                }
                const Correlation c = Correlate(patch, g, probe, x, y, 1);
                if (c.visibleFraction >= tuning.minimumVisibleFraction && c.score > bestScore) {
                    bestScore = c.score;
                    bestX = x;
                    bestY = y;
                }
            }
        }
        // Sub-pixel parabolic refinement on each axis.
        const auto refine = [&](bool horizontal) {
            const double sx = horizontal ? 1.0 : 0.0;
            const double sy = horizontal ? 0.0 : 1.0;
            const Correlation left = Correlate(patch, g, probe, bestX - sx, bestY - sy, 1);
            const Correlation right = Correlate(patch, g, probe, bestX + sx, bestY + sy, 1);
            if (left.visibleFraction < tuning.minimumVisibleFraction ||
                right.visibleFraction < tuning.minimumVisibleFraction || left.score <= -1.0 || right.score <= -1.0) {
                return 0.0;
            }
            const double minus = left.score, plus = right.score;
            const double denominator = minus - 2.0 * bestScore + plus;
            if (denominator < -1e-9) {
                return std::clamp(0.5 * (minus - plus) / denominator, -0.5, 0.5);
            }
            return 0.0;
        };
        const double subX = refine(true);
        const double subY = refine(false);
        const Correlation refined = Correlate(patch, g, probe, bestX + subX, bestY + subY, 1);
        if (refined.visibleFraction >= tuning.minimumVisibleFraction && refined.score >= bestScore) {
            bestScore = refined.score;
            bestX += subX;
            bestY += subY;
        }
    }
    match.score = bestScore;
    match.matched = bestScore >= tuning.minimumScore;
    match.sceneOffset = {bestX * g.sceneUnitsPerPixelX, bestY * g.sceneUnitsPerPixelY};
    return match;
}

QVector<LineCheck> CheckLines(const QVector<LineReference>& lines, const ProbeFrame& probe,
                              const MatchTuning& tuning, double budgetMs,
                              const std::atomic<bool>* cancelled)
{
    QVector<LineCheck> checks;
    checks.reserve(lines.size());
    if (probe.resident && probe.IsValid()) {
        checks.resize(lines.size());
        for (int i = 0; i < lines.size(); ++i) checks[i].lineId = lines[i].lineId;
        QElapsedTimer timer;
        timer.start();
        constexpr int batchSize = 64;
        for (int offset = 0; offset < lines.size(); offset += batchSize) {
            if ((cancelled && cancelled->load(std::memory_order_relaxed)) ||
                (budgetMs > 0.0 && timer.nsecsElapsed() / 1e6 >= budgetMs)) break;
            const int count = std::min(batchSize, int(lines.size()) - offset);
            QVector<ReferencePatch> patches;
            QVector<QPointF> seeds;
            for (int i = 0; i < count; ++i) {
                patches.push_back(lines[offset + i].reference);
                seeds.push_back(lines[offset + i].lastSceneOffset);
            }
            const auto matches = probe.resident->Matches(probe.stamp, patches, tuning, seeds, cancelled);
            if (matches.size() != count) break;
            for (int i = 0; i < count; ++i) {
                checks[offset + i].checked = true;
                checks[offset + i].match = matches[i];
            }
        }
        return checks;
    }
    QElapsedTimer timer;
    timer.start();
    bool stopped = !probe.IsValid();
    for (const LineReference& line : lines) {
        LineCheck check;
        check.lineId = line.lineId;
        if (!stopped && ((cancelled && cancelled->load(std::memory_order_relaxed)) ||
                         (budgetMs > 0.0 && timer.nsecsElapsed() / 1e6 >= budgetMs))) {
            stopped = true;
        }
        if (!stopped) {
            check.checked = true;
            const bool hasSeed = line.lastSceneOffset != QPointF();
            const auto seed = hasSeed ? std::optional<QPointF>(line.lastSceneOffset) : std::nullopt;
            check.match = probe.resident
                ? probe.resident->Match(probe.stamp, line.reference, tuning, seed)
                : MatchPatch(line.reference, probe, tuning, seed);
        }
        checks.push_back(check);
    }
    return checks;
}

QVector<PreparedLine> PrepareLines(const RecognitionResult& result, double minimumWordHeightPx)
{
    QVector<PreparedLine> prepared;
    prepared.reserve(result.output.lines.size());
    QVector<QRectF> referenceRects;
    const SourceStamp& stamp = result.stamp;
    const double width = stamp.viewportWidth;
    const double height = stamp.viewportHeight;
    for (const RecognizedLine& line : result.output.lines) {
        PreparedLine out;
        bool unmappedSpan = false;
        for (const RecognizedWord& word : line.words) {
            const QString text = word.text.trimmed();
            const QRectF scene = ViewRectToScene(stamp.viewTransform, width, height, word.boundsPx);
            const QRectF protectedScene = ViewRectToScene(stamp.viewTransform, width, height,
                word.protectedBoundsPx.isEmpty() ? word.boundsPx : word.protectedBoundsPx);
            TokenDisposition disposition = word.disposition;
            const auto usableRect = [](const QRectF& r) {
                return !r.isEmpty() && std::isfinite(r.x()) && std::isfinite(r.y()) &&
                       std::isfinite(r.width()) && std::isfinite(r.height());
            };
            if (!usableRect(scene) || !usableRect(protectedScene)) {
                disposition = TokenDisposition::kInvalidGeometry;
                unmappedSpan = true;
            } else if (disposition == TokenDisposition::kCandidate) {
                if (!std::isfinite(word.confidence) || word.confidence < .7 || word.confidence > 1.0001)
                    disposition = TokenDisposition::kLowConfidence;
                else if (text.isEmpty()) disposition = TokenDisposition::kUnrecognized;
                else if (word.boundsPx.height() < minimumWordHeightPx)
                    disposition = TokenDisposition::kTooSmall;
                else if (IsHighRiskToken(text)) disposition = TokenDisposition::kHighRisk;
            }
            out.words.push_back({text, scene, disposition, word.confidence, protectedScene});
            if (usableRect(protectedScene))
                out.sceneRect = out.sceneRect.isNull() ? protectedScene : out.sceneRect.united(protectedScene);
        }
        const bool trackable=!out.words.isEmpty()&&!unmappedSpan;
        referenceRects.push_back(trackable?out.sceneRect:QRectF());
        if(trackable&&!result.reference.resident)out.reference=BuildReferencePatch(result.reference,out.sceneRect);
        prepared.push_back(std::move(out));
    }
    if(result.reference.resident){
        const auto references=result.reference.resident->References(stamp,referenceRects);
        if(references.size()==prepared.size())
            for(int i=0;i<prepared.size();++i)prepared[i].reference=references[i];
    }
    return prepared;
}

std::optional<double> SceneSimilarity(const ProbeFrame& a, const ProbeFrame& b)
{
    if (a.resident && b.resident)
        return a.resident->Similarity(a.stamp, *b.resident, b.stamp);
    if (a.resident || b.resident) return std::nullopt;
    if (!a.IsValid() || !b.IsValid()) {
        return std::nullopt;
    }
    const QRectF unit(0.0, 0.0, 1.0, 1.0);
    const QRectF va = ViewRectToScene(a.stamp.viewTransform, a.luma.width(), a.luma.height(),
                                      QRectF(0, 0, a.luma.width(), a.luma.height()))
                          .intersected(unit);
    const QRectF vb = ViewRectToScene(b.stamp.viewTransform, b.luma.width(), b.luma.height(),
                                      QRectF(0, 0, b.luma.width(), b.luma.height()))
                          .intersected(unit);
    const QRectF common = va.intersected(vb);
    if (common.isEmpty() || va.isEmpty() ||
        common.width() * common.height() < 0.25 * va.width() * va.height()) {
        return std::nullopt;
    }
    constexpr int kColumns = 48;
    constexpr int kRows = 27;
    const double cellW = common.width() / kColumns;
    const double cellH = common.height() / kRows;
    const double halfAX = 0.5 * cellW * PixelsPerSceneX(a);
    const double halfAY = 0.5 * cellH * PixelsPerSceneY(a);
    const double halfBX = 0.5 * cellW * PixelsPerSceneX(b);
    const double halfBY = 0.5 * cellH * PixelsPerSceneY(b);
    double sumA = 0, sumB = 0, sumAA = 0, sumBB = 0, sumAB = 0;
    int n = 0;
    for (int r = 0; r < kRows; ++r) {
        for (int c = 0; c < kColumns; ++c) {
            const QPointF scene(common.x() + (c + 0.5) / kColumns * common.width(),
                                common.y() + (r + 0.5) / kRows * common.height());
            const QPointF pa = SceneToViewPixel(a.stamp.viewTransform, a.luma.width(), a.luma.height(), scene);
            const QPointF pb = SceneToViewPixel(b.stamp.viewTransform, b.luma.width(), b.luma.height(), scene);
            float x = 0.0f, y = 0.0f;
            if (!BoxAverage(a, pa.x(), pa.y(), halfAX, halfAY, x) ||
                !BoxAverage(b, pb.x(), pb.y(), halfBX, halfBY, y)) {
                continue;
            }
            sumA += x;
            sumB += y;
            sumAA += static_cast<double>(x) * x;
            sumBB += static_cast<double>(y) * y;
            sumAB += static_cast<double>(x) * y;
            ++n;
        }
    }
    if (n < 64) {
        return std::nullopt;
    }
    const double varA = sumAA - sumA * sumA / n;
    const double varB = sumBB - sumB * sumB / n;
    if (varA < n * 16.0 && varB < n * 16.0) {
        // Two flat views: similar only when their brightness agrees.
        return std::abs(sumA / n - sumB / n) < 8.0 ? 1.0 : 0.0;
    }
    if (varA <= 1e-9 || varB <= 1e-9) {
        return 0.0;
    }
    return (sumAB - sumA * sumB / n) / std::sqrt(varA * varB);
}

bool IsHighRiskToken(const QString& text)
{
    const QString token = text.trimmed();
    if (token.isEmpty()) {
        return true;
    }
    static const QString kStandaloneConfusables = QStringLiteral("lI|10Oo");
    if (token.size() == 1 && kStandaloneConfusables.contains(token.at(0))) {
        return true;
    }
    static const QString kConfusableLetters = QStringLiteral("OoIlSsBZzGgq|");
    static const QString kPlainPunctuation =
        QStringLiteral(".,;:!?'\"()[]-/&%$+=*#@’‘“”–—€£");
    bool hasDigit = false;
    bool hasConfusableLetter = false;
    for (const QChar ch : token) {
        if (ch.isDigit()) {
            // Superscript/subscript and other non-ASCII digit forms are notation.
            if (ch.unicode() > 0x7f) {
                return true;
            }
            hasDigit = true;
            continue;
        }
        if (ch.isLetter()) {
            if (ch.category() == QChar::Letter_Modifier) {
                return true;
            }
            hasConfusableLetter = hasConfusableLetter || kConfusableLetters.contains(ch);
            continue;
        }
        if (!kPlainPunctuation.contains(ch)) {
            return true; // math operators, superscripts, symbols: never claimed
        }
    }
    return hasDigit && hasConfusableLetter;
}

// --- Scheduler ---------------------------------------------------------------

struct RecognitionScheduler::State {
    struct Job {
        std::uint64_t id{};
        std::uint64_t authority{};
        SourceStamp stamp;
        QImage pixels;
        SharedDeviceImage device;
        CorroborationRequest corroboration;
    };

    std::mutex mutex;
    std::condition_variable wake;
    std::condition_variable exitedSignal;
    std::mutex callbackMutex;
    bool callbacksEnabled{true};
    std::unique_ptr<Recognizer> recognizer;
    Completion completion;
    StatusChanged statusChanged;
    int referenceDecimation{2};
    bool probeOnly{};
    double minimumWordHeightPx{7.0};
    std::optional<Job> pending;
    bool inFlight{};
    std::atomic<bool> cancelInFlight{false};
    std::uint64_t authority{1};
    std::uint64_t nextJobId{1};
    bool shutdown{};
    bool exited{};
    BackendStatus status;
    SchedulerStats stats;
    std::thread thread;

    void Publish(const BackendStatus& value)
    {
        {
            std::lock_guard lock(mutex);
            status = value;
        }
        std::lock_guard callbackLock(callbackMutex);
        if (callbacksEnabled && statusChanged) {
            statusChanged(value);
        }
    }

    std::uint64_t Enqueue(const SourceStamp& stamp, QImage owned, SharedDeviceImage device = {},
                          CorroborationRequest corroboration = {})
    {
        std::lock_guard lock(mutex);
        if (shutdown || status.state == BackendState::kUnavailable ||
            status.state == BackendState::kFailed) {
            return 0;
        }
        if (pending) {
            ++stats.coalesced;
        }
        Job job;
        job.id = nextJobId++;
        job.authority = authority;
        job.stamp = stamp;
        job.pixels = std::move(owned);
        job.device = std::move(device);
        job.corroboration = std::move(corroboration);
        const std::uint64_t id = job.id;
        pending = std::move(job);
        ++stats.submitted;
        wake.notify_one();
        return id;
    }

    static void Run(std::shared_ptr<State> self)
    {
        BackendStatus initial;
        if(self->probeOnly)initial.state=BackendState::kReady;
        else if (self->recognizer) {
            try {initial = self->recognizer->Initialize();}
            catch(const std::exception& error){initial.state=BackendState::kFailed;initial.detail=QString::fromUtf8(error.what());}
        } else {
            initial.state = BackendState::kUnavailable;
            initial.detail = QStringLiteral("No recognition backend is available.");
        }
        self->Publish(initial);
        for (;;) {
            Job job;
            {
                std::unique_lock lock(self->mutex);
                self->wake.wait(lock, [&] { return self->shutdown || self->pending.has_value(); });
                if (self->shutdown) {
                    break;
                }
                job = std::move(*self->pending);
                self->pending.reset();
                self->inFlight = true;
                self->cancelInFlight.store(job.authority != self->authority);
                ++self->stats.started;
            }
            QElapsedTimer timer;
            timer.start();
            RecognitionResult result;
            result.jobId = job.id;
            result.authority = job.authority;
            result.stamp = job.stamp;
            result.deviceInput = static_cast<bool>(job.device);
            QImage pixels=job.pixels;
            try {
            const BackendState state = [&] {
                std::lock_guard lock(self->mutex);
                return self->status.state;
            }();
            if (state != BackendState::kReady) {
                result.output.error = QStringLiteral("Recognition backend is not ready.");
            } else if(self->probeOnly) {
                result.output.inputFault=true;
                if(job.device && job.device->HasResidentAnalysis()) {
                    result.output.ok=job.device->PrepareResidentAnalysis(result,
                        self->referenceDecimation, true, self->cancelInFlight, result.output.error);
                } else if(job.device)pixels=job.device->ReadProbe(self->cancelInFlight,result.output.error);
                else result.output.error="Device validation requires an owned image.";
                result.output.ok=result.reference.IsValid() || !pixels.isNull();
                result.output.cancelled=self->cancelInFlight.load();
                result.output.inputFault=!result.output.ok;
            } else {
                const bool wantsDevice = job.device && self->recognizer->WantsDeviceInput();
                if(job.device && !wantsDevice) {
                    result.output.inputFault=true;
                    pixels=job.device->Readback(self->cancelInFlight,result.output.error);
                    result.output.inputFault=pixels.isNull();
                }
                if(!job.device || wantsDevice || !pixels.isNull())result.output = wantsDevice
                    ? self->recognizer->RecognizeDevice(*job.device, self->cancelInFlight)
                    : self->recognizer->Recognize(pixels, self->cancelInFlight);
                else {result.output.cancelled=self->cancelInFlight.load();result.output.inputFault=true;}
                const auto currentStatus = self->recognizer->Status();
                if (currentStatus.state != BackendState::kNotStarted) self->Publish(currentStatus);
            }
            if (result.output.ok && !self->cancelInFlight.load()) {
                const bool resident = pixels.isNull() && job.device && job.device->HasResidentAnalysis();
                if (resident && !self->probeOnly) {
                    result.output.ok=job.device->PrepareResidentAnalysis(result,
                        self->referenceDecimation, false, self->cancelInFlight, result.output.error);
                    result.output.inputFault=!result.output.ok;
                }
                // Only an explicit CPU implementation/fallback downloads pixels.
                // A resident-analysis error never retries via image readback.
                if (!resident) {
                if(pixels.isNull()&&job.device){
                    result.output.inputFault=true;pixels=job.device->Readback(self->cancelInFlight,result.output.error);
                    result.output.inputFault=pixels.isNull();
                }
                if(pixels.isNull()){result.output.ok=false;result.output.inputFault=true;result.output.cancelled=self->cancelInFlight.load();}
                // Camera acquisition identity already comes from its owner.
                // Validation-only results are never temporal OCR evidence.
                // Hash only saved/unknown recognition inputs, on this worker.
                if (!self->probeOnly && !job.stamp.liveCameraCapture)
                    result.sourcePixelDigest = SourcePixelDigest(pixels);
                result.reference = MakeProbeFrame(pixels.constBits(), pixels.width(),
                                                  pixels.height(),
                                                  static_cast<int>(pixels.bytesPerLine()),
                                                  self->referenceDecimation, job.stamp);
                for (RecognizedLine& line : result.output.lines) {
                    QRectF bounds;
                    for (const RecognizedWord& word : line.words) {
                        bounds = bounds.isNull() ? word.boundsPx : bounds.united(word.boundsPx);
                    }
                    const LineColors colors = MeasureLineColors(pixels, bounds.toAlignedRect());
                    result.lineColors.push_back(colors);
                    for (RecognizedWord& word : line.words) {
                        if (word.protectedBoundsPx.isEmpty()) word.protectedBoundsPx = word.boundsPx;
                        if (word.paddedBounds && word.disposition == TokenDisposition::kCandidate)
                            word.boundsPx = RefineWordInkBounds(pixels, word, colors);
                    }
                }
                }
                if (result.output.ok && !self->cancelInFlight.load()) {
                    result.prepared = PrepareLines(result, self->minimumWordHeightPx);
                    result.corroboratedJobId = job.corroboration.selectedJobId;
                    result.corroborationTuning = job.corroboration.tuning;
                    if(result.reference.resident){
                        const auto matches=result.reference.resident->Matches(result.stamp,job.corroboration.references,
                            job.corroboration.tuning,{},&self->cancelInFlight);
                        for(const auto& match:matches)result.corroborationMatches.push_back(match.matched);
                    } else for (const auto& patch : job.corroboration.references)
                        result.corroborationMatches.push_back(MatchPatch(patch,result.reference,job.corroboration.tuning).matched);
                }
            }
            }catch(const std::exception& error){result.output.ok=false;result.output.lines.clear();result.output.error=QString::fromUtf8(error.what());}
            result.durationMs = timer.nsecsElapsed() / 1e6;
            bool current = false;
            {
                std::lock_guard lock(self->mutex);
                self->inFlight = false;
                current = job.authority == self->authority && !self->shutdown;
                if (current) {
                    ++self->stats.completed;
                } else {
                    ++self->stats.revoked;
                }
            }
            if (current) {
                std::lock_guard callbackLock(self->callbackMutex);
                if (self->callbacksEnabled && self->completion) {
                    self->completion(std::move(result));
                }
            }
            // job.pixels is released only here, after the native call returned.
        }
        std::lock_guard lock(self->mutex);
        self->exited = true;
        self->exitedSignal.notify_all();
    }
};

RecognitionScheduler::RecognitionScheduler(std::unique_ptr<Recognizer> recognizer,
                                           Completion completion,
                                           StatusChanged statusChanged,
                                           int referenceDecimation,
                                           double minimumWordHeightPx, bool probeOnly)
    : state_(std::make_shared<State>())
{
    state_->minimumWordHeightPx = minimumWordHeightPx;
    state_->probeOnly=probeOnly;
    state_->recognizer = std::move(recognizer);
    state_->completion = std::move(completion);
    state_->statusChanged = std::move(statusChanged);
    state_->referenceDecimation = std::max(1, referenceDecimation);
    state_->thread = std::thread(&State::Run, state_);
}

RecognitionScheduler::~RecognitionScheduler()
{
    {
        std::lock_guard lock(state_->mutex);
        state_->shutdown = true;
        state_->pending.reset();
        ++state_->authority;
        state_->cancelInFlight.store(true);
    }
    state_->wake.notify_all();
    {
        // Waits for any callback already running, then forbids new ones.
        std::lock_guard callbackLock(state_->callbackMutex);
        state_->callbacksEnabled = false;
    }
    bool exited = false;
    {
        std::unique_lock lock(state_->mutex);
        exited = state_->exitedSignal.wait_for(lock, std::chrono::milliseconds(1500),
                                               [&] { return state_->exited; });
    }
    if (exited) {
        state_->thread.join();
    } else {
        // A held native call keeps its own reference to State and inputs.
        state_->thread.detach();
    }
}

std::uint64_t RecognitionScheduler::Submit(const SourceStamp& stamp, QImage pixels)
{
    if (pixels.isNull()) {
        return 0;
    }
    // Force an independently owned ARGB32 buffer: callers may wrap the
    // presenter's reusable readback memory.
    QImage owned = pixels.format() == QImage::Format_ARGB32
                       ? ToOwnedArgb(pixels.constBits(), pixels.width(), pixels.height(),
                                     static_cast<int>(pixels.bytesPerLine()))
                       : pixels.convertToFormat(QImage::Format_ARGB32).copy();
    if (owned.isNull()) {
        return 0;
    }
    return state_->Enqueue(stamp, std::move(owned));
}

std::uint64_t RecognitionScheduler::Submit(const SourceStamp& stamp, SharedPixels pixels)
{
    // Shared pixels are immutable after hand-off, so a read-only view is an
    // owned input: the worker keeps the buffer alive until it is done.
    QImage view = WrapPixels(pixels);
    if (view.isNull()) {
        return 0;
    }
    return state_->Enqueue(stamp, std::move(view));
}

std::uint64_t RecognitionScheduler::SubmitDevice(const SourceStamp& stamp, SharedDeviceImage device,
                                                  CorroborationRequest corroboration)
{
    if(!device||!device->Data()||device->width<=0||device->height<=0||device->width>8192||device->height>8192||
        static_cast<std::size_t>(device->width)*device->height*4>128ull*1024*1024||
        device->pitch<static_cast<std::size_t>(device->width)*4||
        stamp.sourceEpoch!=device->sourceEpoch||stamp.frameSeq!=device->frameSequence||
        stamp.viewportWidth!=device->width||stamp.viewportHeight!=device->height)return 0;
    return state_->Enqueue(stamp, {}, std::move(device), std::move(corroboration));
}

void RecognitionScheduler::Cancel()
{
    std::lock_guard lock(state_->mutex);
    ++state_->authority;
    if (state_->pending) {
        state_->pending.reset();
        ++state_->stats.revoked;
    }
    state_->cancelInFlight.store(true);
}

std::uint64_t RecognitionScheduler::Authority() const
{
    std::lock_guard lock(state_->mutex);
    return state_->authority;
}

bool RecognitionScheduler::IsCurrent(const RecognitionResult& result) const
{
    return result.authority == Authority();
}

bool RecognitionScheduler::HasWork() const
{
    std::lock_guard lock(state_->mutex);
    return state_->inFlight || state_->pending.has_value();
}

bool RecognitionScheduler::InFlight() const
{
    std::lock_guard lock(state_->mutex);
    return state_->inFlight;
}

BackendStatus RecognitionScheduler::Status() const
{
    std::lock_guard lock(state_->mutex);
    return state_->status;
}

SchedulerStats RecognitionScheduler::Stats() const
{
    std::lock_guard lock(state_->mutex);
    return state_->stats;
}

// --- Probe worker ------------------------------------------------------------

struct ProbeWorker::State {
    std::mutex mutex;
    std::condition_variable wake;
    std::condition_variable exitedSignal;
    std::mutex callbackMutex;
    bool callbacksEnabled{true};
    Completion completion;
    std::optional<std::pair<std::uint64_t, ProbeRequest>> pending;
    std::uint64_t pendingAuthority{};
    bool inFlight{};
    std::atomic<bool> cancelInFlight{false};
    std::uint64_t authority{1};
    std::uint64_t nextJobId{1};
    bool shutdown{};
    bool exited{};
    ProbeWorkerStats stats;
    std::thread thread;

    static void Run(std::shared_ptr<State> self)
    {
        for (;;) {
            std::uint64_t jobId = 0;
            std::uint64_t jobAuthority = 0;
            ProbeRequest request;
            {
                std::unique_lock lock(self->mutex);
                self->wake.wait(lock, [&] { return self->shutdown || self->pending.has_value(); });
                if (self->shutdown) {
                    break;
                }
                jobId = self->pending->first;
                request = std::move(self->pending->second);
                jobAuthority = self->pendingAuthority;
                self->pending.reset();
                self->inFlight = true;
                self->cancelInFlight.store(jobAuthority != self->authority);
            }
            ProbeOutcome outcome;
            outcome.jobId = jobId;
            outcome.authority = jobAuthority;
            outcome.stamp = request.stamp;
            QElapsedTimer timer;
            timer.start();
            outcome.probe=request.preparedProbe;
            if (request.pixels && request.pixels->IsValid()) {
                auto probe = std::make_shared<ProbeFrame>(MakeProbeFrame(
                    request.pixels->bytes.data(), request.pixels->width, request.pixels->height,
                    request.pixels->strideBytes, std::max(1, request.decimation), request.stamp));
                if (probe->IsValid()) {
                    outcome.probe = std::move(probe);
                }
            }
            outcome.probeMs = timer.nsecsElapsed() / 1e6;
            timer.restart();
            if (outcome.probe) {
                if (request.previous) {
                    outcome.similarity = SceneSimilarity(*request.previous, *outcome.probe);
                }
                outcome.checks = CheckLines(request.lines, *outcome.probe, request.tuning, request.budgetMs,
                                            &self->cancelInFlight);
                for (const LineCheck& check : outcome.checks) {
                    outcome.uncheckedLines += check.checked ? 0 : 1;
                }
            }
            outcome.checkMs = timer.nsecsElapsed() / 1e6;
            outcome.pixels = std::move(request.pixels);
            bool current = false;
            {
                std::lock_guard lock(self->mutex);
                self->inFlight = false;
                current = jobAuthority == self->authority && !self->shutdown;
                ++(current ? self->stats.completed : self->stats.revoked);
                self->stats.maxCheckMs = std::max(self->stats.maxCheckMs, outcome.checkMs);
            }
            if (current) {
                std::lock_guard callbackLock(self->callbackMutex);
                if (self->callbacksEnabled && self->completion) {
                    self->completion(std::move(outcome));
                }
            }
        }
        std::lock_guard lock(self->mutex);
        self->exited = true;
        self->exitedSignal.notify_all();
    }
};

ProbeWorker::ProbeWorker(Completion completion)
    : state_(std::make_shared<State>())
{
    state_->completion = std::move(completion);
    state_->thread = std::thread(&State::Run, state_);
}

ProbeWorker::~ProbeWorker()
{
    {
        std::lock_guard lock(state_->mutex);
        state_->shutdown = true;
        state_->pending.reset();
        ++state_->authority;
        state_->cancelInFlight.store(true);
    }
    state_->wake.notify_all();
    {
        std::lock_guard callbackLock(state_->callbackMutex);
        state_->callbacksEnabled = false;
    }
    // A lost GPU can outlive its caller. State and inputs stay worker-owned,
    // callbacks are disabled above, and destruction never joins indefinitely.
    bool exited;
    {
        std::unique_lock lock(state_->mutex);
        exited = state_->exitedSignal.wait_for(lock, std::chrono::milliseconds(1500),
                                               [&] { return state_->exited; });
    }
    if (exited) state_->thread.join();
    else state_->thread.detach();
}

std::uint64_t ProbeWorker::Submit(ProbeRequest request)
{
    std::lock_guard lock(state_->mutex);
    if (state_->shutdown) {
        return 0;
    }
    if (state_->pending) {
        ++state_->stats.coalesced;
    }
    const std::uint64_t id = state_->nextJobId++;
    state_->pending.emplace(id, std::move(request));
    state_->pendingAuthority = state_->authority;
    ++state_->stats.submitted;
    state_->wake.notify_one();
    return id;
}

void ProbeWorker::Cancel()
{
    std::lock_guard lock(state_->mutex);
    ++state_->authority;
    if (state_->pending) {
        state_->pending.reset();
        ++state_->stats.revoked;
    }
    state_->cancelInFlight.store(true);
}

bool ProbeWorker::IsCurrent(const ProbeOutcome& outcome) const
{
    std::lock_guard lock(state_->mutex);
    return outcome.authority == state_->authority;
}

bool ProbeWorker::HasPending() const
{
    std::lock_guard lock(state_->mutex);
    return state_->pending.has_value();
}

bool ProbeWorker::HasWork() const
{
    std::lock_guard lock(state_->mutex);
    return state_->inFlight || state_->pending.has_value();
}

ProbeWorkerStats ProbeWorker::Stats() const
{
    std::lock_guard lock(state_->mutex);
    return state_->stats;
}

// --- Overlay model -----------------------------------------------------------

OverlayModel::OverlayModel(ModelTuning tuning)
    : tuning_(tuning)
{
}

void OverlayModel::Reset()
{
    lines_.clear();
    pending_.clear();
    pendingEvidenceStamp_ = {};
    pendingEvidenceDigest_.clear();
    pendingEvidenceAuthority_ = 0;
    immutable_ = false;
    liveCamera_ = false;
}

AdmitVerdict OverlayModel::Admit(const RecognitionResult& result, const SourceStamp& current,
                                 const ProbeFrame* latestProbe, std::int64_t nowMs)
{
    return AdmitImpl(result, current, latestProbe, nowMs, false);
}

AdmitVerdict OverlayModel::AdmitLiveCamera(const RecognitionResult& result, const SourceStamp& current,
                                           const ProbeFrame* latestProbe, std::int64_t nowMs)
{
    return AdmitImpl(result, current, latestProbe, nowMs, false, nullptr, false, true);
}

AdmitVerdict OverlayModel::AdmitImmutable(const RecognitionResult& result,
                                          const SourceStamp& selectedFrame, std::int64_t nowMs,
                                          const RecognitionResult* corroboration)
{
    if (!result.output.ok || result.output.cancelled) return AdmitVerdict::kFailed;
    if (result.stamp.frameSeq != selectedFrame.frameSeq ||
        !result.reference.stamp.SameScene(result.stamp) ||
        result.reference.stamp.frameSeq != result.stamp.frameSeq) return AdmitVerdict::kStaleScene;
    return AdmitImpl(result, selectedFrame, nullptr, nowMs, true, corroboration);
}

AdmitVerdict OverlayModel::AdmitAdvancingMedia(const RecognitionResult& result,
                                              const SourceStamp& current,
                                              const ProbeFrame* latestProbe,
                                              std::int64_t mediaNowMs)
{
    return AdmitImpl(result, current, latestProbe, mediaNowMs, false, nullptr, true);
}

AdmitVerdict OverlayModel::AdmitImpl(const RecognitionResult& result, const SourceStamp& current,
                                    const ProbeFrame* latestProbe, std::int64_t nowMs, bool immutable,
                                    const RecognitionResult* corroboration, bool advancingMedia, bool liveCamera)
{
    if (!result.output.ok || result.output.cancelled) {
        return AdmitVerdict::kFailed;
    }
    if (!result.stamp.SameScene(current)) {
        return AdmitVerdict::kStaleScene;
    }
    if (!immutable && !advancingMedia && nowMs - result.stamp.timestampMs > tuning_.maxResultAgeMs) {
        return AdmitVerdict::kTooOld;
    }
    if (!result.reference.IsValid() ||
        (result.reference.resident && result.prepared.size() != result.output.lines.size())) {
        // A resident job must arrive fully prepared by its worker. A malformed
        // completion must not turn fixture fallback preparation into UI CUDA.
        return AdmitVerdict::kFailed;
    }
    if (immutable_ != immutable || liveCamera_ != liveCamera) Reset();
    immutable_ = immutable;
    liveCamera_ = liveCamera;
    const SourceStamp& stamp = result.stamp;
    const double jobWidth = stamp.viewportWidth;
    const double jobHeight = stamp.viewportHeight;
    const bool newerProbe = latestProbe && latestProbe->IsValid() &&
                            latestProbe->stamp.SameScene(current) &&
                            latestProbe->stamp.timestampMs > stamp.timestampMs;
    // The scheduler prepares lines on its worker; hand-built results are
    // prepared here.
    const QVector<PreparedLine> prepared =
        result.prepared.size() == result.output.lines.size()
            ? result.prepared
            : PrepareLines(result, tuning_.minimumWordHeightPx);
    const bool independentNeighbor = immutable && corroboration && corroboration->output.ok &&
        !corroboration->output.cancelled && corroboration->authority == result.authority &&
        corroboration->stamp.SameScene(stamp) && corroboration->stamp.frameSeq != stamp.frameSeq &&
        corroboration->sourcePixelDigestKind == result.sourcePixelDigestKind &&
        corroboration->sourcePixelDigest.size() == 32 && result.sourcePixelDigest.size() == 32 &&
        corroboration->sourcePixelDigest != result.sourcePixelDigest && corroboration->reference.IsValid() &&
        corroboration->reference.stamp.SameScene(corroboration->stamp) &&
        corroboration->reference.stamp.frameSeq == corroboration->stamp.frameSeq &&
        (!corroboration->reference.resident ||
         (corroboration->prepared.size() == corroboration->output.lines.size() &&
          result.jobId != 0 && corroboration->corroboratedJobId == result.jobId &&
          corroboration->corroborationMatches.size() == prepared.size() &&
          corroboration->corroborationTuning.minimumScore == tuning_.match.minimumScore &&
          corroboration->corroborationTuning.minimumVisibleFraction == tuning_.match.minimumVisibleFraction &&
          corroboration->corroborationTuning.searchRadiusPx == tuning_.match.searchRadiusPx));
    QHash<QString, QVector<QRectF>> neighborWords;
    if (independentNeighbor) {
        const auto neighbor = corroboration->prepared.size() == corroboration->output.lines.size()
            ? corroboration->prepared : PrepareLines(*corroboration, tuning_.minimumWordHeightPx);
        for (const PreparedLine& line : neighbor)
            for (const PreparedWord& word : line.words)
                if (word.disposition == TokenDisposition::kCandidate)
                    neighborWords[word.text].push_back(word.sceneRect);
    }

    struct Candidate {
        AcceptedLine line;
        const PreparedLine* source{};
        bool matchDeferred{};
    };
    QVector<Candidate> candidates;
    int movedLines = 0;
    QElapsedTimer matchTimer;
    matchTimer.start();
    for (int i = 0; i < prepared.size(); ++i) {
        const PreparedLine& line = prepared[i];
        if (line.words.isEmpty()) {
            continue;
        }
        Candidate candidate;
        candidate.source = &line;
        candidate.line.sceneRect = line.sceneRect;
        candidate.line.reference = line.reference;
        candidate.line.colors = StableLineColors(i < result.lineColors.size() ? result.lineColors[i] : LineColors{});
        candidate.line.provenance = stamp;
        candidate.line.valid = !newerProbe || line.reference.valid;
        candidate.line.validatedAtMs = stamp.timestampMs;
        candidate.line.validatedFrameSeq = stamp.frameSeq;
        if (newerProbe && line.reference.valid) {
            if (latestProbe->resident || matchTimer.nsecsElapsed() / 1e6 >= tuning_.admitMatchBudgetMs) {
                // Budget spent on the UI thread: admit unverified. The line
                // stays hidden until an off-thread probe validates it.
                candidate.line.valid = false;
                candidate.matchDeferred = true;
            } else {
                const PatchMatch m = MatchPatch(candidate.line.reference, *latestProbe, tuning_.match);
                if (m.visible && !m.matched) {
                    ++movedLines;
                    continue;
                }
                if (m.matched) {
                    candidate.line.sceneOffset = m.sceneOffset;
                    candidate.line.validatedAtMs = latestProbe->stamp.timestampMs;
                    candidate.line.validatedFrameSeq = latestProbe->stamp.frameSeq;
                } else {
                    candidate.line.valid = false; // out of the newer view: unverified
                }
            }
        }
        candidates.push_back(std::move(candidate));
    }
    // Temporal agreement: a word is shown only after two independent
    // recognitions of the same scene agree on its text and position. Earlier
    // words are indexed by text so dense pages stay linear, not quadratic.
    QHash<QString, QVector<QRectF>> earlier;
    for (const AcceptedLine& line : lines_) {
        if (!line.provenance.SameScene(current)) {
            continue;
        }
        for (const AcceptedWord& word : line.words) {
            earlier[word.text].push_back(word.sceneRect.translated(line.sceneOffset));
        }
    }
    const bool independentPending = !immutable && pendingEvidenceStamp_.SameScene(stamp) &&
        pendingEvidenceAuthority_ == result.authority &&
        pendingEvidenceStamp_.frameSeq != stamp.frameSeq &&
        (liveCamera
            ? pendingEvidenceStamp_.liveCameraCapture && stamp.liveCameraCapture &&
                stamp.frameSeq > pendingEvidenceStamp_.frameSeq
            : pendingDigestKind_ == result.sourcePixelDigestKind &&
                pendingEvidenceDigest_.size() == 32 && result.sourcePixelDigest.size() == 32 &&
                pendingEvidenceDigest_ != result.sourcePixelDigest);
    // Existing accepted words already have independent confirmation. Keep
    // them above, including during a duplicate refresh. Only NEW words need
    // a distinct prior observation. Live capture supplies a new acquisition
    // sequence even on a byte-identical steady/virtual scene. Saved/unknown
    // inputs still require different complete pixel hashes. Changing only a
    // job ID/timestamp or repainting one capture never supplies evidence.
    if (independentPending) {
        for (const PendingWord& word : pending_) {
            if (word.sourceEpoch == current.sourceEpoch &&
                word.geometryGeneration == current.geometryGeneration) {
                earlier[word.text].push_back(word.sceneRect);
            }
        }
    }
    const auto confirmedBefore = [&](const QString& text, const QRectF& currentRect) {
        if (immutable) return true; // selectable result, separate from glyph eligibility
        const auto it = earlier.constFind(text);
        if (it == earlier.constEnd()) {
            return false;
        }
        for (const QRectF& rect : it.value()) {
            if (Iou(rect, currentRect) >= tuning_.agreementIou) {
                return true;
            }
        }
        return false;
    };

    QVector<AcceptedLine> admitted;
    QSet<std::uint64_t> retainedIds;
    QVector<PendingWord> nextPending;
    for (Candidate& candidate : candidates) {
        // A registration failure never promotes a candidate into a glyph.
        // Preserve the confirmed reading text, with visible uncertainty, only
        // on the sample's own freshness clock.
        bool neighborRegistered = false;
        if (independentNeighbor && candidate.line.reference.valid &&
            matchTimer.nsecsElapsed() / 1e6 < tuning_.admitMatchBudgetMs)
        {
            const auto index = static_cast<qsizetype>(candidate.source - prepared.constData());
            neighborRegistered = corroboration->reference.resident
                ? (index < corroboration->corroborationMatches.size() &&
                   corroboration->corroborationMatches[index])
                : MatchPatch(candidate.line.reference, corroboration->reference, tuning_.match).matched;
        }
        if (!candidate.line.reference.valid) candidate.line.readingTokens.push_back({});
        for (const PreparedWord& word : candidate.source->words) {
            const QRectF currentRect = word.sceneRect.translated(candidate.line.sceneOffset);
            if (word.disposition != TokenDisposition::kCandidate || IsHighRiskToken(word.text)) {
                ++candidate.line.omittedWords;
                candidate.line.readingTokens.push_back({});
                if (!word.protectedSceneRect.isEmpty())
                    candidate.line.protectedSceneRects.push_back(word.protectedSceneRect);
                continue;
            }
            if (confirmedBefore(word.text, currentRect)) {
                const auto evidence = neighborWords.constFind(word.text);
                const bool glyphEligible = !immutable || (neighborRegistered && evidence != neighborWords.constEnd() &&
                    std::any_of(evidence->begin(), evidence->end(), [&](const QRectF& r) {
                        return Iou(word.sceneRect, r) >= tuning_.agreementIou;
                    }));
                candidate.line.words.push_back({word.text, word.sceneRect, glyphEligible});
                candidate.line.readingTokens.push_back(word.text);
                if (!glyphEligible) candidate.line.protectedSceneRects.push_back(
                    word.protectedSceneRect.isEmpty() ? word.sceneRect : word.protectedSceneRect);
            } else {
                ++candidate.line.omittedWords;
                candidate.line.readingTokens.push_back({});
                candidate.line.protectedSceneRects.push_back(
                    word.protectedSceneRect.isEmpty() ? word.sceneRect : word.protectedSceneRect);
                nextPending.push_back({word.text, currentRect, current.sourceEpoch,
                                       current.geometryGeneration});
            }
        }
        if (immutable && candidate.line.reference.valid &&
            std::any_of(candidate.line.words.begin(), candidate.line.words.end(),
                [](const AcceptedWord& word) { return !word.glyphEligible; }))
            candidate.line.readingTokens.prepend(QString());
        if (!candidate.line.readingTokens.isEmpty()) {
            bool retained = false;
            bool duplicate = false;
            if (latestProbe) {
                // Keep the already validated object when re-recognition only
                // jitters its boxes/reference. Do NOT transfer proof to the new
                // pixels or coordinates: retain the old reference, geometry,
                // line identity and validation clock together. The next probe
                // still checks that object and real content changes remove it.
                for (const AcceptedLine& old : lines_) {
                    if (!old.valid || !old.provenance.SameScene(current) ||
                        old.words.size() != candidate.line.words.size() ||
                        old.readingTokens != candidate.line.readingTokens ||
                        old.protectedSceneRects != candidate.line.protectedSceneRects ||
                        old.reference.valid != candidate.line.reference.valid) {
                        continue;
                    }
                    bool sameWords = true;
                    for (int i = 0; i < old.words.size(); ++i) {
                        const QRectF a = old.words[i].sceneRect.translated(old.sceneOffset);
                        const QRectF b = candidate.line.words[i].sceneRect.translated(candidate.line.sceneOffset);
                        const double tolerance = std::min(a.height(), b.height()) * 0.1;
                        if (old.words[i].text != candidate.line.words[i].text || Iou(a,b) < 0.8 ||
                            std::abs(a.left()-b.left()) > tolerance ||
                            std::abs(a.top()-b.top()) > tolerance ||
                            std::abs(a.width()-b.width()) > tolerance ||
                            std::abs(a.height()-b.height()) > tolerance) {
                            sameWords = false;
                            break;
                        }
                    }
                    if (sameWords) {
                        candidate.line.colors = StableLineColors(candidate.line.colors, &old.colors);
                        if (!IsShown(old, nowMs, latestProbe->stamp.frameSeq) ||
                            (candidate.line.valid &&
                             (candidate.line.validatedAtMs > old.validatedAtMs ||
                              candidate.line.validatedFrameSeq > old.validatedFrameSeq))) break;
                        if (retainedIds.contains(old.id)) {
                            duplicate = true;
                            break;
                        }
                        const LineColors colors = candidate.line.colors;
                        candidate.line = old;
                        candidate.line.colors = colors;
                        retained = true;
                        retainedIds.insert(old.id);
                        break;
                    }
                }
            }
            if (duplicate) continue;
            if (!retained) candidate.line.id = nextLineId_++;
            admitted.push_back(std::move(candidate.line));
        }
    }

    // This result supersedes accepted lines inside the area it covered.
    const QRectF coverage = ViewRectToScene(stamp.viewTransform, jobWidth, jobHeight,
                                            QRectF(0.0, 0.0, jobWidth, jobHeight));
    QVector<AcceptedLine> kept;
    for (const AcceptedLine& line : lines_) {
        if (!line.provenance.SameScene(current)) {
            continue;
        }
        if (!coverage.contains(line.sceneRect.translated(line.sceneOffset).center())) {
            // A cropped OCR line can replace words of a wider cached line
            // whose centre is outside coverage. Retaining both would revive
            // duplicate words when a later full-view probe validates them.
            // Retire the whole old line conservatively, without inventing
            // reading tokens for its unrecognized/offscreen remainder.
            // The new result can contain only rejected spans or no spans.
            // Neither permits falling back to old accepted words in its view.
            const bool replaced = std::any_of(line.words.begin(), line.words.end(),
                [&](const AcceptedWord& word) {
                    return coverage.intersects(word.sceneRect.translated(line.sceneOffset));
                }) || std::any_of(line.protectedSceneRects.begin(), line.protectedSceneRects.end(),
                [&](const QRectF& r) { return coverage.intersects(r.translated(line.sceneOffset)); });
            if (!replaced) kept.push_back(line);
        }
    }
    kept += admitted;
    lines_ = std::move(kept);
    pending_ = std::move(nextPending);
    pendingEvidenceStamp_ = stamp;
    pendingEvidenceDigest_ = result.sourcePixelDigest.size() == 32 ? result.sourcePixelDigest : QByteArray{};
    pendingDigestKind_ = result.sourcePixelDigestKind;
    pendingEvidenceAuthority_ = result.authority;
    if (std::any_of(admitted.begin(), admitted.end(), [&](const AcceptedLine& line) {
        return !immutable ? !line.words.isEmpty() : line.reference.valid &&
            std::any_of(line.words.begin(), line.words.end(), [](const AcceptedWord& word) { return word.glyphEligible; });
    })) {
        return AdmitVerdict::kAccepted;
    }
    if (immutable && !admitted.isEmpty()) return AdmitVerdict::kPending;
    return pending_.isEmpty() ? (movedLines > 0 ? AdmitVerdict::kContentMoved : AdmitVerdict::kEmpty)
                              : AdmitVerdict::kPending;
}

QVector<LineReference> OverlayModel::LineReferences(const SourceStamp& scene) const
{
    QVector<const AcceptedLine*> ordered;
    for (const AcceptedLine& line : lines_) {
        if (line.provenance.SameScene(scene) && line.reference.valid) ordered.push_back(&line);
    }
    std::stable_sort(ordered.begin(), ordered.end(), [](const auto* a, const auto* b) {
        if (a->valid != b->valid) return !a->valid;
        return a->validatedAtMs < b->validatedAtMs;
    });
    QVector<LineReference> references;
    references.reserve(ordered.size());
    for (const auto* line : ordered) {
        references.push_back({line->id, line->reference, line->sceneOffset});
    }
    return references;
}

ValidationSummary OverlayModel::ApplyChecks(const QVector<LineCheck>& checks, const SourceStamp& probeStamp)
{
    ValidationSummary summary;
    QHash<std::uint64_t, const LineCheck*> byId;
    byId.reserve(checks.size());
    for (const LineCheck& check : checks) {
        byId.insert(check.lineId, &check);
    }
    QVector<AcceptedLine> kept;
    kept.reserve(lines_.size());
    for (AcceptedLine& line : lines_) {
        if (!line.provenance.SameScene(probeStamp)) {
            ++summary.invalid;
            continue;
        }
        const LineCheck* check = byId.value(line.id, nullptr);
        // Not part of this probe (admitted after it was taken), or already
        // validated against newer pixels: keep the newer state.
        if (!check || probeStamp.timestampMs < line.validatedAtMs) {
            kept.push_back(line);
            continue;
        }
        if (!check->checked) {
            // A missed budget is not evidence that previously verified text
            // changed. Keep its existing proof without renewing it; IsShown's
            // unchanged freshness deadline hides it if checks remain delayed.
            ++summary.unchecked;
            kept.push_back(line);
            continue;
        }
        ++summary.checked;
        if (!check->match.visible) {
            line.valid = false;
            ++summary.notVisible;
            kept.push_back(line);
            continue;
        }
        if (!check->match.matched) {
            ++summary.invalid; // content changed, occluded or moved too far
            continue;
        }
        line.valid = true;
        line.sceneOffset = check->match.sceneOffset;
        line.validatedAtMs = probeStamp.timestampMs;
        line.validatedFrameSeq = probeStamp.frameSeq;
        ++summary.valid;
        kept.push_back(line);
    }
    lines_ = std::move(kept);
    return summary;
}

ValidationSummary OverlayModel::Validate(const ProbeFrame& probe, std::int64_t nowMs)
{
    Q_UNUSED(nowMs);
    if (!probe.IsValid() || probe.resident) {
        // Resident validation arrives through worker-computed ApplyChecks.
        return {};
    }
    return ApplyChecks(CheckLines(LineReferences(probe.stamp), probe, tuning_.match, 0.0), probe.stamp);
}

bool OverlayModel::IsShown(const AcceptedLine& line, std::int64_t nowMs, std::uint64_t currentFrameSeq) const
{
    return line.reference.valid &&
           std::any_of(line.words.begin(), line.words.end(), [](const AcceptedWord& word) { return word.glyphEligible; }) &&
           IsReadable(line, nowMs, currentFrameSeq);
}

bool OverlayModel::IsReadable(const AcceptedLine& line, std::int64_t nowMs, std::uint64_t currentFrameSeq) const
{
    return line.valid && !line.readingTokens.isEmpty() &&
           (nowMs - line.validatedAtMs <= tuning_.freshnessMs || currentFrameSeq == line.validatedFrameSeq);
}

QVector<SceneWord> OverlayModel::SceneWords(std::int64_t nowMs, std::uint64_t currentFrameSeq) const
{
    QVector<SceneWord> words;
    QVector<QRectF> protectedRects;
    for (const AcceptedLine& line : lines_) {
        for (const QRectF& r : line.protectedSceneRects)
            protectedRects.push_back(r.translated(line.sceneOffset));
        if (!IsShown(line, nowMs, currentFrameSeq))
            for (const AcceptedWord& word : line.words)
                protectedRects.push_back(word.sceneRect.translated(line.sceneOffset));
    }
    for (const AcceptedLine& line : lines_) {
        if (!IsShown(line, nowMs, currentFrameSeq)) {
            continue;
        }
        for (int i = 0; i < line.words.size(); ++i) {
            const AcceptedWord& word = line.words[i];
            if (!word.glyphEligible) continue;
            words.push_back({word.text, word.sceneRect.translated(line.sceneOffset), line.colors, line.id,
                             protectedRects, i});
        }
    }
    return words;
}

QVector<DisplayWord> OverlayModel::DisplayWords(const ViewTransform& transform, double width,
                                                double height, std::int64_t nowMs,
                                                std::uint64_t currentFrameSeq) const
{
    QVector<DisplayWord> words;
    if (!UsableTransform(transform, width, height)) {
        return words;
    }
    const QRectF surface(0.0, 0.0, width, height);
    for (const SceneWord& word : SceneWords(nowMs, currentFrameSeq)) {
        const QRectF view = SceneRectToView(transform, width, height, word.sceneRect);
        if (view.isEmpty() || !surface.intersects(view)) {
            continue;
        }
        words.push_back({word.text, view, word.colors, word.lineId});
    }
    return words;
}

QVector<const AcceptedLine*> OrderLinesForReading(QVector<const AcceptedLine*> lines)
{
    struct Entry {
        QRectF rect;
        const AcceptedLine* line{};
        int row{};
    };
    QVector<Entry> entries;
    entries.reserve(lines.size());
    for (const AcceptedLine* line : lines) {
        if (line) entries.push_back({line->sceneRect.translated(line->sceneOffset), line, 0});
    }
    // 1) Strict order by vertical centre (ties: left edge, then id).
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        const double ay = a.rect.center().y();
        const double by = b.rect.center().y();
        if (ay != by) {
            return ay < by;
        }
        if (a.rect.left() != b.rect.left()) {
            return a.rect.left() < b.rect.left();
        }
        return a.line->id < b.line->id;
    });
    // 2) Explicit row clustering: a line joins the current row while its
    //    centre lies inside the row anchor's vertical extent. The anchor is
    //    the row's first line, so membership never depends on pair order.
    int row = -1;
    double anchorBottom = 0.0;
    for (Entry& entry : entries) {
        if (row < 0 || entry.rect.center().y() > anchorBottom) {
            ++row;
            anchorBottom = entry.rect.bottom();
        }
        entry.row = row;
    }
    // 3) Strict order within rows by left edge (ties: id).
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        if (a.row != b.row) {
            return a.row < b.row;
        }
        if (a.rect.left() != b.rect.left()) {
            return a.rect.left() < b.rect.left();
        }
        return a.line->id < b.line->id;
    });
    QVector<const AcceptedLine*> ordered;
    ordered.reserve(entries.size());
    for (const Entry& entry : entries) ordered.push_back(entry.line);
    return ordered;
}

QString OverlayModel::ReadingText(std::int64_t nowMs, std::uint64_t currentFrameSeq) const
{
    // The pane also preserves exact-source recognition when registration is
    // unavailable; its leading omission marks that original ink is retained.
    QVector<const AcceptedLine*> readable;
    for (const AcceptedLine& line : lines_) {
        if (IsReadable(line, nowMs, currentFrameSeq)) {
            readable.push_back(&line);
        }
    }
    QStringList out;
    for (const AcceptedLine* line : OrderLinesForReading(std::move(readable))) {
        QStringList words;
        bool previousOmission = false;
        for (const QString& token : line->readingTokens) {
            if (token.isEmpty()) {
                if (!previousOmission) {
                    words.push_back(QStringLiteral("\u2026"));
                }
                previousOmission = true;
            } else {
                words.push_back(token);
                previousOmission = false;
            }
        }
        out.push_back(words.join(QLatin1Char(' ')));
    }
    return out.join(QLatin1Char('\n'));
}

QVector<ScreenLine> OverlayModel::ScreenLines(std::int64_t nowMs, std::uint64_t currentFrameSeq,
                                              const ScreenTextLimits& limits) const
{
    // Exactly the lines SceneWords() draws. Reading-only immutable evidence
    // and every unconfirmed span stay "\u2026": a screen reader must never
    // hear text that the overlay did not replace on screen.
    QVector<const AcceptedLine*> shown;
    for (const AcceptedLine& line : lines_) {
        if (IsShown(line, nowMs, currentFrameSeq)) shown.push_back(&line);
    }
    const QString ellipsis(QChar(0x2026));
    const int maxLine = std::max(3, limits.maxLineUtf16);
    QVector<ScreenLine> out;
    int total = 0;
    for (const AcceptedLine* line : OrderLinesForReading(std::move(shown))) {
        if (out.size() >= limits.maxLines) break;
        // readingTokens holds one non-empty token per confirmed word, in
        // order; anything else is an inconsistency and fails closed.
        QVector<int> raw;
        raw.reserve(line->readingTokens.size());
        int next = 0;
        bool consistent = true;
        for (const QString& token : line->readingTokens) {
            if (token.isEmpty()) {
                raw.push_back(-1);
                continue;
            }
            if (next >= line->words.size() || line->words[next].text != token) {
                consistent = false;
                break;
            }
            raw.push_back(line->words[next].glyphEligible ? next : -1);
            ++next;
        }
        if (!consistent || next != line->words.size()) continue;

        ScreenLine screen;
        screen.lineId = line->id;
        screen.sceneRect = line->sceneRect.translated(line->sceneOffset);
        const auto append = [&screen](int wordIndex, const QString& piece) {
            if (!screen.text.isEmpty()) screen.text += QLatin1Char(' ');
            screen.tokens.push_back({wordIndex, static_cast<int>(screen.text.size()),
                                     static_cast<int>(piece.size())});
            screen.text += piece;
        };
        for (int wordIndex : raw) {
            if (wordIndex < 0 && !screen.tokens.isEmpty() && screen.tokens.constLast().wordIndex < 0)
                continue; // consecutive omissions collapse into one run
            const QString& piece = wordIndex < 0 ? ellipsis : line->words[wordIndex].text;
            if (piece.isEmpty()) continue;
            const int separator = screen.text.isEmpty() ? 0 : 1;
            if (screen.text.size() + separator + piece.size() > maxLine) {
                screen.truncated = true;
                break;
            }
            append(wordIndex, piece);
        }
        if (screen.truncated) {
            // Make room for the trailing marker by dropping whole words.
            while (!screen.tokens.isEmpty() && screen.tokens.constLast().wordIndex >= 0 &&
                   screen.text.size() + 2 > maxLine) {
                const int start = screen.tokens.constLast().start;
                screen.tokens.removeLast();
                screen.text.truncate(std::max(0, start - 1));
            }
            if (!screen.tokens.isEmpty() && screen.tokens.constLast().wordIndex >= 0)
                append(-1, ellipsis);
        }
        if (std::none_of(screen.tokens.begin(), screen.tokens.end(),
                         [](const ScreenToken& token) { return token.wordIndex >= 0; }))
            continue;
        if (total + screen.text.size() > limits.maxTotalUtf16) break;
        total += static_cast<int>(screen.text.size());
        out.push_back(std::move(screen));
    }
    return out;
}

int OverlayModel::AcceptedWordCount() const
{
    int count = 0;
    for (const AcceptedLine& line : lines_) {
        count += line.words.size();
    }
    return count;
}

bool OverlayModel::HasAcceptedText() const
{
    return AcceptedWordCount() > 0;
}

} // namespace okuflow::uct

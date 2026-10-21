#include "okuflow/common/ultra_clear_text.hpp"
#include "okuflow/common/uct_region_planner.hpp"
#include "uct_color_policy.hpp"
#include <QCryptographicHash>

#include <QElapsedTimer>
#include <QHash>
#include <QSet>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace okuflow::uct {
namespace {

timing::Values TimingOutput(const RecognitionOutput& output, bool device, bool current,
                            std::uint64_t relatedJob = 0)
{
    std::uint32_t flags = (output.cancelled ? timing::Cancelled : 0u) |
        (output.inputFault ? timing::InputFault : 0u) | (output.backendPreparing ? timing::Preparing : 0u) |
        (output.compositeInput ? timing::Composite : 0u) | (device ? timing::DeviceInput : 0u) |
        (current ? timing::Current : timing::Stale) | (relatedJob ? timing::Corroboration : 0u);
    if (!output.ok && !output.cancelled && !output.inputFault && !output.backendPreparing && !output.compositeInput)
        flags |= timing::Failed;
    return {0, static_cast<std::uint32_t>(output.lines.size()), flags, relatedJob};
}

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
    using namespace color_policy;
    LineColors colors;
    const QRect box = bounds.intersected(image.rect());
    if (box.isEmpty()) return colors;
    long long lumaSum = 0;
    unsigned count = 0;
    for (int y = box.top(); y <= box.bottom(); y += 2)
        for (int x = box.left(); x <= box.right(); x += 2) {
            lumaSum += Luma(image.constScanLine(y) + x * 4);
            ++count;
        }
    if (!count) return colors;
    const int threshold = static_cast<int>(lumaSum / count);
    unsigned histogram[2][3][256]{}, lumaHistogram[2][256]{}, bodyCounts[2]{};
    for (int y = box.top(); y <= box.bottom(); y += 2)
        for (int x = box.left(); x <= box.right(); x += 2) {
            const uchar* px = image.constScanLine(y) + x * 4;
            const int value = Luma(px), side = value < threshold ? 0 : 1;
            ++bodyCounts[side]; ++lumaHistogram[side][value];
            for (int channel = 0; channel < 3; ++channel)
                ++histogram[side][channel][px[2 - channel]];
        }
    if (!bodyCounts[0] || !bodyCounts[1]) return colors;
    Rgb body[2];
    int endpoint[2];
    for (int side = 0; side < 2; ++side) {
        for (int channel = 0; channel < 3; ++channel)
            body[side].channel[channel] = Median(histogram[side][channel], bodyCounts[side]);
        endpoint[side] = Endpoint(lumaHistogram[side], bodyCounts[side], side);
    }
    unsigned cornerVotes[2]{};
    if (box.width() >= 3 && box.height() >= 3)
        for (int corner = 0; corner < 4; ++corner) {
            const int left = corner % 2 ? box.right() - 2 : box.left();
            const int top = corner / 2 ? box.bottom() - 2 : box.top();
            unsigned matches[2]{};
            for (int y = top; y < top + 3; ++y)
                for (int x = left; x < left + 3; ++x) {
                    const uchar* px = image.constScanLine(y) + x * 4;
                    const int side = Luma(px) < threshold ? 0 : 1;
                    if (Compatible(px, body[side])) ++matches[side];
                }
            for (int side = 0; side < 2; ++side)
                if (matches[side] >= 5) ++cornerVotes[side];
        }
    // First consult the immediately adjacent 1px perimeter. Dense E/H-like
    // glyphs can touch all four in-box corners: those corners are ink, while
    // the touching exterior is still paper. A button's 1px padding is also
    // local paper, independently of the page beyond its narrow boundary.
    const QRect adjacent[4]{
        QRect(box.left(), box.top() - 1, box.width(), box.top() > 0 ? 1 : 0),
        QRect(box.left(), box.bottom() + 1, box.width(), box.bottom() + 1 < image.height() ? 1 : 0),
        QRect(box.left() - 1, box.top(), box.left() > 0 ? 1 : 0, box.height()),
        QRect(box.right() + 1, box.top(), box.right() + 1 < image.width() ? 1 : 0, box.height())};
    std::memset(histogram, 0, sizeof(histogram));
    unsigned sideCounts[4][2]{}, sideTotals[4]{}, ringCounts[2]{};
    const auto sampleSides = [&](const QRect (&sides)[4]) {
        for (int s = 0; s < 4; ++s)
            for (int y = sides[s].top(); y <= sides[s].bottom(); y += 2)
                for (int x = sides[s].left(); x <= sides[s].right(); x += 2) {
                    const uchar* px = image.constScanLine(y) + x * 4;
                    const int side = Luma(px) < threshold ? 0 : 1;
                    ++sideTotals[s];
                    if (!Compatible(px, body[side])) continue;
                    ++sideCounts[s][side]; ++ringCounts[side];
                    for (int channel = 0; channel < 3; ++channel)
                        ++histogram[side][channel][px[2 - channel]];
                }
    };
    sampleSides(adjacent);
    int bg = AdjacentBackground(sideCounts, sideTotals);
    Rgb paper;
    if (bg >= 0) {
        for (int channel = 0; channel < 3; ++channel)
            paper.channel[channel] = Median(histogram[bg][channel], ringCounts[bg]);
    } else {
        // Each expanded exterior side has one vote, never an area-weighted
        // vote. Exclude expanded corners and require local RGB agreement.
        const int margin = std::clamp(box.height() / 6, 2, 12);
        const QRect sides[4]{
            QRect(box.left(), std::max(0, box.top() - margin), box.width(),
                  box.top() - std::max(0, box.top() - margin)),
            QRect(box.left(), box.bottom() + 1, box.width(),
                  std::min(image.height(), box.bottom() + 1 + margin) - box.bottom() - 1),
            QRect(std::max(0, box.left() - margin), box.top(),
                  box.left() - std::max(0, box.left() - margin), box.height()),
            QRect(box.right() + 1, box.top(),
                  std::min(image.width(), box.right() + 1 + margin) - box.right() - 1, box.height())};
        std::memset(histogram, 0, sizeof(histogram));
        std::memset(sideCounts, 0, sizeof(sideCounts));
        std::memset(sideTotals, 0, sizeof(sideTotals));
        std::memset(ringCounts, 0, sizeof(ringCounts));
        sampleSides(sides);
        bool useRing = false;
        bg = Background(bodyCounts, sideCounts, sideTotals, cornerVotes, useRing);
        paper = body[bg];
        if (useRing && ringCounts[bg] >= 8)
            for (int channel = 0; channel < 3; ++channel)
                paper.channel[channel] = Median(histogram[bg][channel], ringCounts[bg]);
    }
    const int fg = 1 - bg;
    std::memset(histogram, 0, sizeof(histogram));
    unsigned inkCount = 0;
    for (int y = box.top(); y <= box.bottom(); y += 2)
        for (int x = box.left(); x <= box.right(); x += 2) {
            const uchar* px = image.constScanLine(y) + x * 4;
            const int value = Luma(px);
            if ((value < threshold ? 0 : 1) != fg ||
                (fg == 0 ? value > endpoint[fg] : value < endpoint[fg])) continue;
            ++inkCount;
            for (int channel = 0; channel < 3; ++channel)
                ++histogram[fg][channel][px[2 - channel]];
        }
    colors.foreground = QColor(Median(histogram[fg][0], inkCount),
                               Median(histogram[fg][1], inkCount),
                               Median(histogram[fg][2], inkCount));
    colors.background = QColor(paper.channel[0], paper.channel[1], paper.channel[2]);
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

std::optional<std::uint64_t> EvidenceAcquisition(const SourceStamp& stamp)
{
    // The same consistency ApplyHoldProvenance enforces, checked again here
    // so a hand-built or stale stamp cannot name an acquisition it never had.
    switch (stamp.holdDisplay) {
    case HoldDisplay::NotHeld:
        return stamp.frameSeq;
    case HoldDisplay::Live:
        if (stamp.holdAcquisition == stamp.frameSeq) return stamp.frameSeq;
        break;
    case HoldDisplay::Held:
        if (stamp.holdAcquisition < stamp.frameSeq) return stamp.holdAcquisition;
        break;
    case HoldDisplay::Crossfade:
    case HoldDisplay::Unknown:
        break;
    }
    return std::nullopt;
}

void ApplyHoldProvenance(SourceStamp& stamp, const HoldProvenance& hold)
{
    // Live pixels are exactly this capture's frame; held pixels were acquired
    // strictly earlier. Anything else cannot name its acquisition.
    const bool consistent =
        (hold.display == HoldDisplay::Live && hold.acquisition == stamp.frameSeq) ||
        (hold.display == HoldDisplay::Held && hold.acquisition < stamp.frameSeq) ||
        hold.display == HoldDisplay::Crossfade;
    stamp.holdDisplay = consistent ? hold.display : HoldDisplay::Unknown;
    stamp.holdAcquisition =
        consistent && hold.display != HoldDisplay::Crossfade ? hold.acquisition : 0;
}

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
    const int radius = std::clamp(tuning.searchRadiusPx, 0, 10);
    double bestScore = zero.score;
    double bestX = 0.0, bestY = 0.0;
    // Static fast path: an unmoved, unchanged line is the common case.
    if (radius > 0 && zero.score < confident) {
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
            out.words.push_back({text, scene, disposition, word.confidence, protectedScene, word.colors});
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

namespace {
// A paused source does not move. Its *neighbour* may have scrolled: validate
// the same recognized word there, without moving the selected-frame glyph.
// GPU references/samples remain resident and only bounded verdicts return.
QVector<QVector<bool>> MatchImmutableWords(const CorroborationRequest& request,
                                          const RecognitionResult& neighbor,
                                          const std::atomic<bool>& cancelled, bool& failed)
{
    failed = false;
    const auto unavailable = [&]() -> QVector<QVector<bool>> { failed = true; return {}; };
    QVector<QVector<bool>> out;
    if (!request.selectedJobId || !request.selectedReference.IsValid() ||
        !request.selectedReference.stamp.SameScene(neighbor.stamp) ||
        request.selectedReference.stamp.frameSeq == neighbor.stamp.frameSeq ||
        std::abs(double(request.selectedReference.stamp.timestampMs) - neighbor.stamp.timestampMs) > 250 ||
        request.selectedLines.size() > 1024) return out;
    qsizetype wordCount = 0;
    for (const auto& line : request.selectedLines) wordCount += line.words.size();
    if (wordCount > 8192) return out;
    QHash<QString, QVector<QRectF>> recognized;
    for (qsizetype i = 0; i < neighbor.prepared.size(); ++i) {
        if (neighbor.stamp.temporallySmoothed &&
            (neighbor.currentInputMatches.size() != neighbor.prepared.size() ||
             !neighbor.currentInputMatches[i])) continue;
        for (const auto& word : neighbor.prepared[i].words)
            if (word.disposition == TokenDisposition::kCandidate && !IsHighRiskToken(word.text))
                recognized[word.text].push_back(word.sceneRect);
    }
    QVector<QRectF> rects;
    QVector<QPointF> offsets;
    QVector<QPair<int, int>> destinations;
    for (int i = 0; i < request.selectedLines.size(); ++i) {
        const auto& line = request.selectedLines[i];
        out.push_back(QVector<bool>(line.words.size(), false));
        for (int j = 0; j < line.words.size(); ++j) {
            if (cancelled.load()) return unavailable();
            const auto& word = line.words[j];
            if (word.disposition != TokenDisposition::kCandidate || IsHighRiskToken(word.text)) continue;
            const auto evidence = recognized.constFind(word.text);
            if (evidence == recognized.constEnd()) continue;
            // The ordinary completed line proof already covers static words.
            if (i < neighbor.corroborationMatches.size() && neighbor.corroborationMatches[i] &&
                std::any_of(evidence->begin(), evidence->end(), [&](const QRectF& r) {
                    return Iou(word.sceneRect, r) >= .5;
                })) continue;
            std::optional<QPointF> best;
            double nearest = std::numeric_limits<double>::max();
            for (const auto& target : *evidence) {
                const auto delta = target.center() - word.sceneRect.center();
                if (std::abs(delta.x()) > .25 || std::abs(delta.y()) > .25 ||
                    std::min(target.width(), word.sceneRect.width()) < .75 * std::max(target.width(), word.sceneRect.width()) ||
                    std::min(target.height(), word.sceneRect.height()) < .75 * std::max(target.height(), word.sceneRect.height())) continue;
                const double distance = delta.x() * delta.x() + delta.y() * delta.y();
                if (distance < nearest) { nearest = distance; best = delta; }
            }
            if (!best) continue;
            rects.push_back(word.sceneRect);
            offsets.push_back(*best);
            destinations.push_back({i, j});
        }
    }
    // Stronger than ordinary line tracking, with only two probe pixels of
    // local OCR-box refinement around the explicitly recognized destination.
    constexpr MatchTuning tuning{.94, .95, 2};
    for (qsizetype first = 0; first < rects.size(); first += 1024) {
        if (cancelled.load()) return unavailable();
        const auto batch = rects.mid(first, 1024);
        QVector<ReferencePatch> patches;
        if (request.selectedReference.resident)
            patches = request.selectedReference.resident->References(request.selectedReference.stamp, batch);
        else for (const auto& rect : batch) patches.push_back(BuildReferencePatch(request.selectedReference, rect));
        if (patches.size() != batch.size()) return unavailable();
        for (int i = 0; i < patches.size(); ++i) patches[i].sceneRect.translate(offsets[first + i]);
        QVector<PatchMatch> matches;
        if (neighbor.reference.resident)
            matches = neighbor.reference.resident->Matches(neighbor.stamp, patches, tuning, {}, &cancelled);
        else for (const auto& patch : patches) {
            if (cancelled.load()) return unavailable();
            matches.push_back(MatchPatch(patch, neighbor.reference, tuning));
        }
        if (matches.size() != patches.size()) return unavailable();
        for (int i = 0; i < matches.size(); ++i) {
            const auto [line, word] = destinations[first + i];
            out[line][word] = matches[i].matched;
        }
    }
    return out;
}
} // namespace

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
        std::optional<QRectF> shadowFocus;
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
    bool shadowRequested{}; // immutable after starting the worker
    std::unique_ptr<RegionShadowPlanner> shadowPlanner;
    std::optional<QRectF> shadowFocus;
    std::uint64_t shadowAuthority{}; // worker-only
    std::atomic<bool> shadowFault{};
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
    const std::uint64_t timingProducer{timing::NewProducerId()};
    timing::Key activeTiming;

    timing::Channel TimingChannel() const { return probeOnly ? timing::Channel::DeviceValidation : timing::Channel::Recognition; }
    // Called only under the existing scheduler mutex; tracing uses try-lock
    // and cannot block/cancel/reorder the worker's actual jobs.
    void TraceCancellation(std::uint32_t flags) {
        if (!timing::Enabled()) return;
        if (pending) TraceTiming(timing::Event::Cancel, timingProducer, TimingChannel(),
            pending->stamp, pending->id, pending->authority, {0, 0, flags | timing::Cancelled});
        if (activeTiming.jobId) timing::RecordEvent(timing::Event::Cancel, activeTiming,
            {0, 0, flags | timing::Cancelled});
    }

    void Publish(const BackendStatus& value)
    {
        {
            std::lock_guard lock(mutex);
            status = value;
            if (timing::Enabled()) {
                auto key = activeTiming;
                key.producer = timingProducer;
                key.channel = TimingChannel();
                timing::RecordEvent(timing::Event::BackendState, key,
                    {static_cast<std::uint32_t>(value.state), 0,
                        (value.state != BackendState::kNotStarted ? timing::BackendKnown : 0u) |
                        (value.usesGpu ? timing::GpuBackend : 0u)});
            }
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
            TraceTiming(timing::Event::Cancel, timingProducer, TimingChannel(), pending->stamp,
                pending->id, pending->authority, {0, 0, timing::Cancelled | timing::Coalesced});
            ++stats.coalesced;
        }
        Job job;
        job.id = nextJobId++;
        job.authority = authority;
        job.stamp = stamp;
        job.pixels = std::move(owned);
        job.device = std::move(device);
        job.corroboration = std::move(corroboration);
        if (shadowRequested) job.shadowFocus = shadowFocus;
        const std::uint64_t id = job.id;
        TraceTiming(timing::Event::Submit, timingProducer, TimingChannel(), stamp, id, authority,
            {0, 0, (job.device ? timing::DeviceInput : 0u) |
                (job.corroboration.selectedJobId ? timing::Corroboration : 0u), job.corroboration.selectedJobId});
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
                if (timing::Enabled()) self->activeTiming = TimingKey(self->timingProducer,
                    self->TimingChannel(), job.stamp, job.id, job.authority);
            }
            TraceTiming(timing::Event::Dequeue, self->timingProducer, self->TimingChannel(),
                job.stamp, job.id, job.authority, {0, 0, job.device ? timing::DeviceInput : 0u,
                                                job.corroboration.selectedJobId});
            QElapsedTimer timer;
            timer.start();
            RecognitionResult result;
            std::uint32_t timingBackendFlags{};
            result.jobId = job.id;
            result.authority = job.authority;
            result.stamp = job.stamp;
            result.deviceInput = static_cast<bool>(job.device);
            QImage pixels=job.pixels;
            QImage validationPixels;
            try {
            // Extra Stable identity comes only from this capture's own device
            // record, read after its copy completes (compact metadata only).
            // Without one, a stamp cannot keep claiming live or held pixels.
            if (job.device && job.device->HasHoldProvenance()) {
                QString holdError;
                ApplyHoldProvenance(job.stamp,
                    job.device->ReadHoldProvenance(self->cancelInFlight, holdError));
            } else if (job.stamp.holdDisplay != HoldDisplay::NotHeld) {
                ApplyHoldProvenance(job.stamp, {HoldDisplay::Unknown, 0, 0.0f});
            }
            result.stamp = job.stamp;
            const BackendState state = [&] {
                std::lock_guard lock(self->mutex);
                if (timing::Enabled()) timingBackendFlags =
                    (self->status.state != BackendState::kNotStarted ? timing::BackendKnown : 0u) |
                    (self->status.usesGpu ? timing::GpuBackend : 0u);
                return self->status.state;
            }();
            if (state != BackendState::kReady) {
                result.output.error = QStringLiteral("Recognition backend is not ready.");
            } else if (!self->probeOnly && !EvidenceAcquisition(job.stamp)) {
                // A recovery crossfade (or unreadable record) is a composite of
                // two acquisitions: no inference, no evidence, and neither a
                // backend nor an input failure. Validation may still probe it.
                result.output.compositeInput = true;
                result.output.error = QStringLiteral("Extra Stable composite pixels are not recognized.");
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
                if(!job.device || wantsDevice || !pixels.isNull()) {
                    if (timing::Enabled()) timingBackendFlags |= timing::InferenceAttempted;
                    result.output = wantsDevice
                        ? self->recognizer->RecognizeDevice(*job.device, self->cancelInFlight)
                        : self->recognizer->Recognize(pixels, self->cancelInFlight);
                }
                else {result.output.cancelled=self->cancelInFlight.load();result.output.inputFault=true;}
                const auto currentStatus = self->recognizer->Status();
                if (timing::Enabled() && currentStatus.state != BackendState::kNotStarted) {
                    timingBackendFlags &= ~(timing::BackendKnown | timing::GpuBackend);
                    timingBackendFlags |= timing::BackendKnown | (currentStatus.usesGpu ? timing::GpuBackend : 0u);
                }
                if (currentStatus.state != BackendState::kNotStarted) self->Publish(currentStatus);
            }
            if (result.output.ok && !self->cancelInFlight.load()) {
                // The owned analysis image chooses provenance preparation,
                // independently of the recognizer. A CPU bridge may need its
                // existing image readback for OCR while the same selected
                // lease still supplies resident references/colors and the
                // canonical GPU-tree fingerprint used by preferred inference.
                const bool resident = job.device && job.device->HasResidentAnalysis();
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
                const QImage* referencePixels = &pixels;
                if (!self->probeOnly && job.device && job.device->kind == AnalysisKind::TemporallySmoothed) {
                    validationPixels = job.device->ReadValidation(self->cancelInFlight, result.output.error);
                    if (validationPixels.isNull()) { result.output.ok = false; result.output.inputFault = true; }
                    else referencePixels = &validationPixels;
                }
                // Camera acquisition identity already comes from its owner.
                // Validation-only results are never temporal OCR evidence.
                // Hash only saved/unknown recognition inputs, on this worker.
                if (!self->probeOnly && !job.stamp.liveCameraCapture)
                    result.sourcePixelDigest = SourcePixelDigest(*referencePixels);
                result.reference = MakeProbeFrame(referencePixels->constBits(), referencePixels->width(),
                                                  referencePixels->height(),
                                                  static_cast<int>(referencePixels->bytesPerLine()),
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
                        word.colors = MeasureLineColors(pixels, word.boundsPx.toAlignedRect());
                        if (word.paddedBounds && word.disposition == TokenDisposition::kCandidate)
                            word.boundsPx = RefineWordInkBounds(pixels, word, *word.colors);
                    }
                }
                }
                if (result.output.ok && !self->cancelInFlight.load()) {
                    result.prepared = PrepareLines(result, self->minimumWordHeightPx);
                    if (!self->probeOnly && job.device &&
                        job.device->kind == AnalysisKind::TemporallySmoothed && !resident) {
                        const auto recognized = MakeProbeFrame(pixels.constBits(), pixels.width(),
                            pixels.height(), static_cast<int>(pixels.bytesPerLine()),
                            self->referenceDecimation, job.stamp);
                        for (const auto& line : result.output.lines) {
                            if (self->cancelInFlight.load()) break;
                            bool matches = true, hasCandidate = false;
                            for (const auto& word : line.words) {
                                if (word.disposition != TokenDisposition::kCandidate) continue;
                                hasCandidate = true;
                                const auto rect = ViewRectToScene(job.stamp.viewTransform,
                                    pixels.width(), pixels.height(), word.boundsPx);
                                const auto patch = BuildReferencePatch(recognized, rect);
                                const auto match = MatchPatch(patch, result.reference, kInputRegistrationTuning);
                                matches &= match.matched && match.sceneOffset.isNull();
                            }
                            result.currentInputMatches.push_back(hasCandidate && matches);
                        }
                    }
                    result.corroboratedJobId = job.corroboration.selectedJobId;
                    result.corroborationTuning = job.corroboration.tuning;
                    if(result.reference.resident){
                        const auto matches=result.reference.resident->Matches(result.stamp,job.corroboration.references,
                            job.corroboration.tuning,{},&self->cancelInFlight);
                        for(const auto& match:matches)result.corroborationMatches.push_back(match.matched);
                    } else for (const auto& patch : job.corroboration.references) {
                        if (self->cancelInFlight.load()) break;
                        result.corroborationMatches.push_back(MatchPatch(patch,result.reference,job.corroboration.tuning).matched);
                    }
                    if (!job.corroboration.selectedLines.isEmpty()) {
                        bool failed = false;
                        result.corroborationWordMatches = MatchImmutableWords(job.corroboration, result, self->cancelInFlight, failed);
                        if (failed) {
                            result.output.ok = false;
                            result.output.cancelled = self->cancelInFlight.load();
                            result.output.inputFault = !result.output.cancelled;
                            result.output.error = QStringLiteral("Paused word registration unavailable.");
                            result.output.lines.clear();
                            result.prepared.clear();
                            result.corroborationMatches.clear();
                            result.corroborationWordMatches.clear();
                        }
                    }
                }
            }
            }catch(const std::exception& error){result.output.ok=false;result.output.lines.clear();result.output.error=QString::fromUtf8(error.what());}
            result.durationMs = timer.nsecsElapsed() / 1e6;
            bool current = false;
            {
                std::lock_guard lock(self->mutex);
                self->inFlight = false;
                if (timing::Enabled()) self->activeTiming = {};
                current = job.authority == self->authority && !self->shutdown;
                if (current) {
                    ++self->stats.completed;
                } else {
                    ++self->stats.revoked;
                }
            }
            const auto traceDone = [&] {
                if (!timing::Enabled()) return;
                auto values = TimingOutput(result.output, result.deviceInput, current, job.corroboration.selectedJobId);
                values.flags |= timingBackendFlags;
                TraceTiming(timing::Event::Done, self->timingProducer, self->TimingChannel(),
                    result.stamp, job.id, job.authority, values);
                if (values.flags & (timing::Failed | timing::InputFault))
                    TraceTiming(timing::Event::Fault, self->timingProducer, self->TimingChannel(),
                        result.stamp, job.id, job.authority, values);
            };
            if (current) {
                if (self->shadowPlanner && result.output.ok && !result.output.cancelled &&
                    !result.output.inputFault && !result.output.backendPreparing && !self->cancelInFlight.load()) {
                    // Observe only complete current jobs. The observer cannot
                    // alter OCR output or any admission decision. A diagnostic
                    // failure disables it without failing the recognition job.
                    QElapsedTimer shadowTimer;
                    shadowTimer.start();
                    try {
                        if (self->shadowFault.exchange(false)) throw std::runtime_error("Injected shadow observer failure.");
                        if (self->shadowAuthority != job.authority) {
                            self->shadowPlanner->Reset(RegionResetReason::kAuthorityChanged);
                            self->shadowAuthority = job.authority;
                        }
                        result.shadowPlan = std::make_shared<const RegionShadowPlan>(
                            self->shadowPlanner->Observe(result, job.shadowFocus));
                    } catch (...) {
                        self->shadowPlanner.reset();
                        result.shadowPlannerError = QStringLiteral("Shadow planner unavailable; full OCR retained.");
                    }
                    result.shadowPlannerMs = shadowTimer.nsecsElapsed() / 1e6;
                }
                traceDone();
                std::lock_guard callbackLock(self->callbackMutex);
                if (self->callbacksEnabled && self->completion) {
                    self->completion(std::move(result));
                }
            }
            else traceDone();
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
                                           double minimumWordHeightPx, bool probeOnly, bool shadowPlanner)
    : state_(std::make_shared<State>())
{
    state_->minimumWordHeightPx = minimumWordHeightPx;
    state_->probeOnly=probeOnly;
    state_->shadowRequested = shadowPlanner && !probeOnly;
    if (state_->shadowRequested) state_->shadowPlanner = std::make_unique<RegionShadowPlanner>();
    state_->recognizer = std::move(recognizer);
    state_->completion = std::move(completion);
    state_->statusChanged = std::move(statusChanged);
    state_->referenceDecimation = std::max(1, referenceDecimation);
    state_->thread = std::thread(&State::Run, state_);
}

void RecognitionScheduler::SetShadowFocus(std::optional<QRectF> sceneRect)
{
    if (!state_->shadowRequested) return;
    std::lock_guard lock(state_->mutex);
    state_->shadowFocus = std::move(sceneRect);
}

void RecognitionScheduler::FailNextShadowObservationForTest()
{
    state_->shadowFault.store(true);
}

RecognitionScheduler::~RecognitionScheduler()
{
    {
        std::lock_guard lock(state_->mutex);
        state_->TraceCancellation(timing::Shutdown);
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

std::uint64_t RecognitionScheduler::Submit(const SourceStamp& stamp, QImage pixels,
                                          CorroborationRequest corroboration)
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
    return state_->Enqueue(stamp, std::move(owned), {}, std::move(corroboration));
}

std::uint64_t RecognitionScheduler::Submit(const SourceStamp& stamp, SharedPixels pixels,
                                          CorroborationRequest corroboration)
{
    // Shared pixels are immutable after hand-off, so a read-only view is an
    // owned input: the worker keeps the buffer alive until it is done.
    QImage view = WrapPixels(pixels);
    if (view.isNull()) {
        return 0;
    }
    return state_->Enqueue(stamp, std::move(view), {}, std::move(corroboration));
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
    state_->TraceCancellation(0);
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

std::uint64_t RecognitionScheduler::TimingProducerId() const noexcept { return state_->timingProducer; }

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
    const std::uint64_t timingProducer{timing::NewProducerId()};
    timing::Key activeTiming;

    void TraceCancellation(std::uint32_t flags) {
        if (!timing::Enabled()) return;
        if (pending) TraceTiming(timing::Event::Cancel, timingProducer, timing::Channel::Probe,
            pending->second.stamp, pending->first, pendingAuthority, {0, 0, flags | timing::Cancelled});
        if (activeTiming.jobId) timing::RecordEvent(timing::Event::Cancel, activeTiming,
            {0, 0, flags | timing::Cancelled});
    }

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
                if (timing::Enabled()) self->activeTiming = TimingKey(self->timingProducer,
                    timing::Channel::Probe, request.stamp, jobId, jobAuthority);
            }
            TraceTiming(timing::Event::Dequeue, self->timingProducer, timing::Channel::Probe,
                request.stamp, jobId, jobAuthority);
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
                if (timing::Enabled()) self->activeTiming = {};
                ++(current ? self->stats.completed : self->stats.revoked);
                self->stats.maxCheckMs = std::max(self->stats.maxCheckMs, outcome.checkMs);
            }
            if (timing::Enabled()) {
                const timing::Values values{static_cast<std::uint32_t>(outcome.uncheckedLines),
                    static_cast<std::uint32_t>(outcome.checks.size()),
                    (current ? timing::Current : timing::Stale) | (outcome.probe ? 0u : timing::InputFault)};
                TraceTiming(timing::Event::Done, self->timingProducer, timing::Channel::Probe,
                    outcome.stamp, jobId, jobAuthority, values);
                if (!outcome.probe) TraceTiming(timing::Event::Fault, self->timingProducer,
                    timing::Channel::Probe, outcome.stamp, jobId, jobAuthority, values);
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
        state_->TraceCancellation(timing::Shutdown);
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
        TraceTiming(timing::Event::Cancel, state_->timingProducer, timing::Channel::Probe,
            state_->pending->second.stamp, state_->pending->first, state_->pendingAuthority,
            {0, 0, timing::Cancelled | timing::Coalesced});
        ++state_->stats.coalesced;
    }
    const std::uint64_t id = state_->nextJobId++;
    state_->pending.emplace(id, std::move(request));
    state_->pendingAuthority = state_->authority;
    TraceTiming(timing::Event::Submit, state_->timingProducer, timing::Channel::Probe,
        state_->pending->second.stamp, id, state_->authority);
    ++state_->stats.submitted;
    state_->wake.notify_one();
    return id;
}

void ProbeWorker::Cancel()
{
    std::lock_guard lock(state_->mutex);
    state_->TraceCancellation(0);
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

std::uint64_t ProbeWorker::TimingProducerId() const noexcept { return state_->timingProducer; }

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
    immutableConflictRects_.clear();
    immutableConflictSaturated_ = false;
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
    const auto& before = pendingEvidenceStamp_;
    const auto& after = result.stamp;
    const auto& a = before.viewTransform;
    const auto& b = after.viewTransform;
    const bool sameInput = immutable_ && before.SameScene(after) &&
        before.frameSeq == after.frameSeq && before.timestampMs == after.timestampMs &&
        before.viewportWidth == after.viewportWidth && before.viewportHeight == after.viewportHeight &&
        a.valid == b.valid && a.sourceX == b.sourceX && a.sourceY == b.sourceY &&
        a.sourceWidth == b.sourceWidth && a.sourceHeight == b.sourceHeight &&
        a.destinationX == b.destinationX && a.destinationY == b.destinationY &&
        a.destinationWidth == b.destinationWidth && a.destinationHeight == b.destinationHeight &&
        before.temporallySmoothed == after.temporallySmoothed &&
        before.historyEpoch == after.historyEpoch && before.seedFrameSeq == after.seedFrameSeq &&
        before.blendFrameCount == after.blendFrameCount && before.temporalAlpha == after.temporalAlpha &&
        before.holdDisplay == after.holdDisplay && before.holdAcquisition == after.holdAcquisition &&
        pendingEvidenceAuthority_ == result.authority &&
        pendingDigestKind_ == result.sourcePixelDigestKind &&
        pendingEvidenceDigest_.size() == 32 && result.sourcePixelDigest.size() == 32 &&
        pendingEvidenceDigest_ == result.sourcePixelDigest;
    QVector<AcceptedLine> previous;
    if (sameInput) {
        for (const auto& line : lines_)
            if (line.provenance.SameScene(after) && line.provenance.frameSeq == after.frameSeq &&
                line.provenance.timestampMs == after.timestampMs && IsShown(line, nowMs, after.frameSeq))
                previous.push_back(line);
    }
    const auto verdict = AdmitImpl(result, selectedFrame, nullptr, nowMs, true, corroboration);
    if (verdict == AdmitVerdict::kFailed || verdict == AdmitVerdict::kStaleScene) return verdict;
    if (!sameInput) {
        immutableConflictRects_.clear();
        immutableConflictSaturated_ = false;
    }
    const bool blocked = ApplyImmutableConflictBlocks(lines_);
    if (previous.isEmpty()) {
        if (blocked || immutableConflictSaturated_) {
            return std::any_of(lines_.begin(), lines_.end(), [&](const auto& line) {
                return IsShown(line, nowMs, after.frameSeq);
            }) ? AdmitVerdict::kAccepted : AdmitVerdict::kContentMoved;
        }
        return verdict;
    }
    return MergeImmutableCoverage(previous, verdict, nowMs, after.frameSeq);
}

bool OverlayModel::ApplyImmutableConflictBlocks(QVector<AcceptedLine>& lines) const
{
    if (immutableConflictSaturated_) { lines.clear(); return true; }
    if (immutableConflictRects_.isEmpty()) return false;
    // At most 8192 exact-input blocks. Typical words inspect only their own
    // vertical neighborhood rather than every conflict elsewhere on the page.
    std::array<QVector<int>, 64> rows;
    const auto range = [](const QRectF& rect) {
        return std::pair{static_cast<int>(std::clamp(std::floor(rect.top() * 64), 0., 63.)),
                         static_cast<int>(std::clamp(std::floor(rect.bottom() * 64), 0., 63.))};
    };
    for (int i = 0; i < immutableConflictRects_.size(); ++i) {
        const auto [first, last] = range(immutableConflictRects_[i]);
        for (int row = first; row <= last; ++row) rows[row].push_back(i);
    }
    bool changed = false;
    QVector<AcceptedLine> retained;
    for (auto& line : lines) {
        auto indexes = line.readingWordIndices;
        if (indexes.isEmpty()) {
            int word = 0;
            for (const auto& token : line.readingTokens) indexes.push_back(token.isEmpty() ? -1 : word++);
        }
        bool lineChanged = false;
        for (qsizetype t = 0; t < indexes.size(); ++t) {
            const int index = indexes[t];
            if (index < 0 || index >= line.words.size()) continue;
            auto& word = line.words[index];
            const QRectF rect = word.sceneRect.translated(line.sceneOffset);
            const auto [first, last] = range(rect);
            bool conflict = false;
            for (int row = first; !conflict && row <= last; ++row)
                for (const auto block : rows[row])
                    if (immutableConflictRects_[block].intersects(rect)) { conflict = true; break; }
            if (!conflict) continue;
            word.glyphEligible = false;
            indexes[t] = -1;
            line.readingTokens[t].clear();
            ++line.omittedWords;
            lineChanged = changed = true;
        }
        if (lineChanged) line.readingWordIndices = std::move(indexes);
        if (std::any_of(line.readingTokens.begin(), line.readingTokens.end(),
                [](const QString& token) { return !token.isEmpty(); })) retained.push_back(std::move(line));
    }
    lines = std::move(retained);
    return changed;
}

AdmitVerdict OverlayModel::MergeImmutableCoverage(const QVector<AcceptedLine>& previous,
                                                 AdmitVerdict verdict, std::int64_t nowMs,
                                                 std::uint64_t selectedFrameSeq)
{
    // A finite union of ONE selected source, never a history of frames. Count
    // retired reading-only slots too: repeated refinement cannot grow indexes,
    // tokens or protected rectangles without bound.
    constexpr qsizetype maximumLines = 1024, maximumWords = 8192;
    constexpr qsizetype maximumTokens = maximumWords * 4, maximumProtected = maximumWords * 4;
    QVector<AcceptedLine> incoming = std::move(lines_);
    QVector<AcceptedLine> retained;
    qsizetype wordCount = 0, tokenCount = 0, protectedCount = 0;
    for (const auto& line : previous) {
        if (retained.size() >= maximumLines || wordCount + line.words.size() > maximumWords ||
            tokenCount + line.readingTokens.size() > maximumTokens ||
            protectedCount + line.protectedSceneRects.size() > maximumProtected) break;
        retained.push_back(line);
        wordCount += line.words.size();
        tokenCount += line.readingTokens.size();
        protectedCount += line.protectedSceneRects.size();
    }
    const auto absolute = [](const AcceptedLine& line, const AcceptedWord& word) {
        return word.sceneRect.translated(line.sceneOffset);
    };
    const auto strongOverlap = [](const QRectF& left, const QRectF& right) {
        const QRectF overlap = left.intersected(right);
        const double smaller = std::min(left.width() * left.height(), right.width() * right.height());
        return smaller > 0 && overlap.width() * overlap.height() >= smaller * .5;
    };
    // Immutable pages can be dense. A fixed vertical index keeps ordinary
    // row-local refinement from comparing every word with every old line.
    std::array<QVector<qsizetype>, 64> rows;
    const auto rowRange = [](const QRectF& rect) {
        return std::pair{static_cast<int>(std::clamp(std::floor(rect.top() * 64), 0., 63.)),
                         static_cast<int>(std::clamp(std::floor(rect.bottom() * 64), 0., 63.))};
    };
    const auto indexLine = [&](qsizetype index) {
        const auto [first, last] = rowRange(retained[index].sceneRect.translated(retained[index].sceneOffset));
        for (int row = first; row <= last; ++row) rows[row].push_back(index);
    };
    const auto nearby = [&](const QRectF& rect) {
        QVector<qsizetype> indexes;
        QSet<qsizetype> seen;
        const auto [first, last] = rowRange(rect);
        for (int row = first; row <= last; ++row)
            for (const auto index : rows[row])
                if (!seen.contains(index)) { seen.insert(index); indexes.push_back(index); }
        std::sort(indexes.begin(), indexes.end());
        return indexes;
    };
    for (qsizetype i = 0; i < retained.size(); ++i) indexLine(i);
    bool conflict = false;
    const auto block = [&](const QRectF& rect) {
        if (std::find(immutableConflictRects_.begin(), immutableConflictRects_.end(), rect) != immutableConflictRects_.end()) return;
        if (immutableConflictRects_.size() >= maximumWords) { immutableConflictSaturated_ = true; return; }
        immutableConflictRects_.push_back(rect);
    };
    for (qsizetype n = 0; n < incoming.size(); ++n) {
        if (!IsShown(incoming[n], nowMs, selectedFrameSeq)) continue;
        for (const auto o : nearby(incoming[n].sceneRect.translated(incoming[n].sceneOffset))) {
            if (!incoming[n].sceneRect.translated(incoming[n].sceneOffset).intersects(
                    retained[o].sceneRect.translated(retained[o].sceneOffset))) continue;
            for (const auto& oldWord : retained[o].words) {
                if (!oldWord.glyphEligible) continue;
                for (const auto& newWord : incoming[n].words) {
                    if (newWord.glyphEligible && oldWord.text != newWord.text &&
                        strongOverlap(absolute(retained[o], oldWord), absolute(incoming[n], newWord))) {
                        conflict = true;
                        block(absolute(retained[o], oldWord));
                        block(absolute(incoming[n], newWord));
                    }
                }
            }
        }
    }
    // A disagreement names only its old/new word regions. Safe sibling words
    // retain their complete objects and indexes; no punctuation/case/letter
    // equivalence is invented. Persisting these blocks prevents a later
    // preferred pair from silently resurrecting the same disputed word.
    if (conflict) {
        ApplyImmutableConflictBlocks(retained);
        ApplyImmutableConflictBlocks(incoming);
        for (auto& row : rows) row.clear();
        for (qsizetype i = 0; i < retained.size(); ++i) indexLine(i);
    }

    const auto tokenIndexes = [](const AcceptedLine& line) {
        if (!line.readingWordIndices.isEmpty()) return line.readingWordIndices;
        QVector<int> indexes;
        indexes.reserve(line.readingTokens.size());
        int word = 0;
        for (const auto& token : line.readingTokens) indexes.push_back(token.isEmpty() ? -1 : word++);
        return indexes;
    };
    // Subtract only independently proved glyph bounds. Uncovered source ink
    // (including punctuation at the edge of an earlier uncertainty box) stays
    // protected. Four rectangles per cut and a shared finite cap bound work.
    const auto unprotect = [&](AcceptedLine& line, const QRectF& proved) {
        QVector<QRectF> next;
        for (const auto& relative : line.protectedSceneRects) {
            const QRectF rect = relative.translated(line.sceneOffset), cut = rect.intersected(proved);
            if (cut.isEmpty()) { next.push_back(relative); continue; }
            const auto add = [&](const QRectF& r) {
                if (!r.isEmpty()) next.push_back(r.translated(-line.sceneOffset));
            };
            add(QRectF(rect.left(), rect.top(), rect.width(), cut.top() - rect.top()));
            add(QRectF(rect.left(), cut.bottom(), rect.width(), rect.bottom() - cut.bottom()));
            add(QRectF(rect.left(), cut.top(), cut.left() - rect.left(), cut.height()));
            add(QRectF(cut.right(), cut.top(), rect.right() - cut.right(), cut.height()));
            if (next.size() > maximumProtected) return false;
        }
        const auto count = protectedCount - line.protectedSceneRects.size() + next.size();
        if (count > maximumProtected) return false;
        protectedCount = count;
        line.protectedSceneRects = std::move(next);
        return true;
    };
    const auto trimProvedOmissions = [](AcceptedLine& line) {
        if (!line.protectedSceneRects.isEmpty() || line.omittedWords != 0 ||
            !std::all_of(line.readingWordIndices.begin(), line.readingWordIndices.end(),
                [&](int index) { return index < 0 || line.words[index].glyphEligible; })) return;
        for (qsizetype t = line.readingWordIndices.size(); t-- > 0;)
            if (line.readingWordIndices[t] < 0) {
                line.readingWordIndices.removeAt(t);
                line.readingTokens.removeAt(t);
            }
    };
    const auto retireReadingOnly = [&](AcceptedLine& line, const QRectF& proved) {
        auto indexes = tokenIndexes(line);
        std::optional<qsizetype> first;
        for (qsizetype t = 0; t < indexes.size(); ++t) {
            const int index = indexes[t];
            if (index >= 0 && index < line.words.size() && !line.words[index].glyphEligible &&
                strongOverlap(absolute(line, line.words[index]), proved)) {
                indexes[t] = -1; line.readingTokens[t].clear();
                if (!first) first = t;
            }
        }
        if (first) line.readingWordIndices = std::move(indexes);
        return first;
    };
    for (qsizetype n = 0; n < incoming.size(); ++n) {
        auto& candidate = incoming[n];
        const auto candidateIndexes = tokenIndexes(candidate);
        QVector<bool> keep(candidate.words.size(), false);
        for (const auto index : candidateIndexes)
            if (index >= 0 && index < keep.size()) keep[index] = true;
        for (qsizetype w = 0; w < candidate.words.size(); ++w) {
            if (!keep[w]) continue;
            const auto& word = candidate.words[w];
            const QRectF rect = absolute(candidate, word);
            const auto neighbors = nearby(rect);
            bool duplicate = false;
            for (const auto o : neighbors) {
                auto& old = retained[o];
                if (!old.sceneRect.translated(old.sceneOffset).intersects(rect)) continue;
                for (auto& existing : old.words)
                    if (existing.glyphEligible && strongOverlap(absolute(old, existing), rect)) {
                        duplicate = true;
                        // Only matching incoming words with their OWN completed
                        // proof may improve color on this exact selected source.
                        // Retain the old ID/index, geometry, reference and clocks;
                        // a color correction is never a new observation/proof.
                        if (word.glyphEligible && IsShown(candidate, nowMs, selectedFrameSeq) &&
                            word.text == existing.text && word.colors)
                            existing.colors = StableLineColors(*word.colors,
                                existing.colors ? &*existing.colors : &old.colors);
                    }
            }
            if (duplicate) { keep[w] = false; continue; }
            AcceptedLine* target = nullptr;
            for (const auto o : neighbors)
                if (retained[o].sceneRect.translated(retained[o].sceneOffset).contains(rect)) {
                    target = &retained[o]; break;
                }
            if (!target) {
                for (const auto o : neighbors) {
                    auto replacement = retained[o];
                    if (!retireReadingOnly(replacement, rect)) continue;
                    if (!word.glyphEligible || !IsShown(candidate, nowMs, selectedFrameSeq)) {
                        keep[w] = false; // no duplicate reading-only refresh or self-promotion
                        continue;
                    }
                    // Boundary jitter may extend beyond the old line. Retire
                    // only its unproved duplicate; the new glyph keeps its own
                    // incoming line identity/reference below, never the old
                    // reference or geometry of neighboring proved words.
                    if (!unprotect(replacement, rect)) { keep[w] = false; continue; }
                    trimProvedOmissions(replacement);
                    tokenCount += replacement.readingTokens.size() - retained[o].readingTokens.size();
                    retained[o] = std::move(replacement);
                }
                continue;
            }
            keep[w] = false; // old reading-only text must not be duplicated either
            if (!word.glyphEligible || !IsShown(candidate, nowMs, selectedFrameSeq) ||
                wordCount >= maximumWords || tokenCount >= maximumTokens) continue;
            // This eligibility came from the incoming word's OWN completed
            // neighbor proof in AdmitImpl. The old reference grants no new
            // agreement; retain old references/geometry/clocks and indexes.
            auto replacement = *target;
            const auto retiredPosition = retireReadingOnly(replacement, rect);
            auto indexes = tokenIndexes(replacement);
            AcceptedWord appended = word;
            appended.sceneRect = rect.translated(-replacement.sceneOffset);
            const int appendedIndex = static_cast<int>(replacement.words.size());
            replacement.words.push_back(std::move(appended));
            qsizetype position = retiredPosition.value_or(indexes.size());
            for (qsizetype t = 0; !retiredPosition && t < indexes.size(); ++t) {
                const int index = indexes[t];
                if (index >= 0 && index < replacement.words.size() &&
                    absolute(replacement, replacement.words[index]).center().x() > rect.center().x()) {
                    position = t;
                    break;
                }
            }
            indexes.insert(position, appendedIndex);
            replacement.readingTokens.insert(position, word.text);
            replacement.readingWordIndices = std::move(indexes);
            if (!unprotect(replacement, rect)) continue;
            // A leading uncertainty marker is unnecessary once every still
            // named token has its own proof and no protected omission remains.
            trimProvedOmissions(replacement);
            tokenCount += replacement.readingTokens.size() - target->readingTokens.size();
            ++wordCount;
            *target = std::move(replacement);
        }
        if (std::none_of(keep.begin(), keep.end(), [](bool value) { return value; })) continue;
        AcceptedLine fragment = candidate;
        fragment.words.clear(); fragment.readingTokens.clear(); fragment.readingWordIndices.clear();
        QRectF bounds;
        for (qsizetype t = 0; t < candidateIndexes.size(); ++t) {
            const int index = candidateIndexes[t];
            if (index < 0) { fragment.readingTokens.push_back({}); continue; }
            if (index >= candidate.words.size() || !keep[index]) continue;
            fragment.words.push_back(candidate.words[index]);
            fragment.readingTokens.push_back(candidate.words[index].text);
            bounds = bounds.united(candidate.words[index].sceneRect);
        }
        if (fragment.words.isEmpty() || retained.size() >= maximumLines ||
            wordCount + fragment.words.size() > maximumWords ||
            tokenCount + fragment.readingTokens.size() > maximumTokens ||
            protectedCount + fragment.protectedSceneRects.size() > maximumProtected) continue;
        fragment.sceneRect = bounds;
        // Incoming uncertainty cannot clip the old independently proved ink.
        bool fits = true;
        protectedCount += fragment.protectedSceneRects.size();
        for (const auto o : nearby(candidate.sceneRect.translated(candidate.sceneOffset))) {
            const auto& old = retained[o];
            for (const auto& word : old.words)
                if (word.glyphEligible && !fragment.protectedSceneRects.isEmpty())
                    fits = unprotect(fragment, absolute(old, word)) && fits;
        }
        if (!fits) { protectedCount -= fragment.protectedSceneRects.size(); continue; }
        wordCount += fragment.words.size(); tokenCount += fragment.readingTokens.size();
        retained.push_back(std::move(fragment));
        indexLine(retained.size() - 1);
    }
    lines_ = std::move(retained);
    if (std::any_of(lines_.begin(), lines_.end(), [&](const auto& line) {
            return IsShown(line, nowMs, selectedFrameSeq);
        })) return AdmitVerdict::kAccepted;
    return conflict ? AdmitVerdict::kContentMoved : verdict;
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
    // Composite (Extra Stable crossfade) or unresolved pixels never become
    // recognition evidence, whatever their frame sequence says.
    const std::optional<std::uint64_t> acquisition = EvidenceAcquisition(result.stamp);
    if (!acquisition) {
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
    if (!immutable && latestProbe && latestProbe->IsValid() &&
        latestProbe->stamp.SameScene(current) &&
        latestProbe->stamp.frameSeq >= stamp.frameSeq &&
        !EvidenceAcquisition(latestProbe->stamp)) {
        return AdmitVerdict::kContentMoved; // an older OCR completion cannot revive crossfade text
    }
    const double jobWidth = stamp.viewportWidth;
    const double jobHeight = stamp.viewportHeight;
    // A composite (crossfade/unknown) probe names no acquisition, so it can
    // never be the newer independent check that validates or moves new text.
    const bool newerProbe = latestProbe && latestProbe->IsValid() &&
                            latestProbe->stamp.SameScene(current) &&
                            EvidenceAcquisition(latestProbe->stamp).has_value() &&
                            latestProbe->stamp.timestampMs > stamp.timestampMs;
    // The scheduler prepares lines on its worker; hand-built results are
    // prepared here.
    const QVector<PreparedLine> prepared =
        result.prepared.size() == result.output.lines.size()
            ? result.prepared
            : PrepareLines(result, tuning_.minimumWordHeightPx);
    const bool workerRegistration = corroboration &&
        corroboration->prepared.size() == corroboration->output.lines.size() &&
        result.jobId != 0 && corroboration->corroboratedJobId == result.jobId &&
        corroboration->corroborationMatches.size() == prepared.size() &&
        corroboration->corroborationTuning.minimumScore == tuning_.match.minimumScore &&
        corroboration->corroborationTuning.minimumVisibleFraction == tuning_.match.minimumVisibleFraction &&
        corroboration->corroborationTuning.searchRadiusPx == tuning_.match.searchRadiusPx;
    const bool workerWordRegistration = workerRegistration &&
        corroboration->corroborationWordMatches.size() == prepared.size() &&
        [&] {
            for (int i = 0; i < prepared.size(); ++i)
                if (corroboration->corroborationWordMatches[i].size() != prepared[i].words.size()) return false;
            return true;
        }();
    const bool independentNeighbor = immutable && corroboration && corroboration->output.ok &&
        !corroboration->output.cancelled && corroboration->authority == result.authority &&
        corroboration->stamp.SameScene(stamp) && corroboration->stamp.frameSeq != stamp.frameSeq &&
        corroboration->sourcePixelDigestKind == result.sourcePixelDigestKind &&
        corroboration->sourcePixelDigest.size() == 32 && result.sourcePixelDigest.size() == 32 &&
        corroboration->sourcePixelDigest != result.sourcePixelDigest && corroboration->reference.IsValid() &&
        corroboration->reference.stamp.SameScene(corroboration->stamp) &&
        corroboration->reference.stamp.frameSeq == corroboration->stamp.frameSeq &&
        (workerRegistration || (!corroboration->reference.resident &&
            corroboration->corroboratedJobId == 0 && corroboration->corroborationMatches.isEmpty() &&
            corroboration->corroborationWordMatches.isEmpty()));
    QHash<QString, QVector<QRectF>> neighborWords;
    if (independentNeighbor) {
        const auto neighbor = corroboration->prepared.size() == corroboration->output.lines.size()
            ? corroboration->prepared : PrepareLines(*corroboration, tuning_.minimumWordHeightPx);
        for (qsizetype i = 0; i < neighbor.size(); ++i) {
            if (corroboration->stamp.temporallySmoothed &&
                (corroboration->currentInputMatches.size() != neighbor.size() ||
                 !corroboration->currentInputMatches[i])) continue;
            const auto& line = neighbor[i];
            for (const PreparedWord& word : line.words)
                if (word.disposition == TokenDisposition::kCandidate)
                    neighborWords[word.text].push_back(word.sceneRect);
        }
    }

    struct Candidate {
        AcceptedLine line;
        const PreparedLine* source{};
        bool matchDeferred{};
    };
    QVector<Candidate> candidates;
    QVector<QRectF> failedRegistrationRects;
    int movedLines = 0;
    QElapsedTimer matchTimer;
    matchTimer.start();
    for (int i = 0; i < prepared.size(); ++i) {
        const PreparedLine& line = prepared[i];
        if (line.words.isEmpty()) {
            continue;
        }
        const bool currentInputMatches = !stamp.temporallySmoothed ||
            (result.currentInputMatches.size() == prepared.size() && result.currentInputMatches[i]);
        if (!currentInputMatches) {
            // No new evidence: do not transfer recognition or its clock. A
            // separately current-validated old line may survive below.
            failedRegistrationRects.push_back(line.sceneRect);
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
        if (stamp.temporallySmoothed && !immutable) {
            // Smoothed OCR pixels are never their own proof: the line stays
            // hidden until a single-frame probe at or after this frame
            // matches its current-frame reference (inline below, or a later
            // off-thread ApplyChecks). Two agreeing smoothed reads therefore
            // confirm words into pending/accepted state but draw nothing.
            candidate.line.valid = false;
            candidate.matchDeferred = true;
        }
        if (newerProbe && candidate.line.reference.valid) {
            if (latestProbe->resident || matchTimer.nsecsElapsed() / 1e6 >= tuning_.admitMatchBudgetMs ||
                (stamp.temporallySmoothed && latestProbe->stamp.frameSeq < stamp.frameSeq)) {
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
                    candidate.line.valid = true;
                    candidate.matchDeferred = false;
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
            // A reading-only word is one unconfirmed observation; it can be
            // confirmed through the pending ticket below, never by itself.
            if (!word.glyphEligible) continue;
            earlier[word.text].push_back(word.sceneRect.translated(line.sceneOffset));
        }
    }
    // Evidence is the acquisition the pixels show. An Extra Stable held image
    // repeats its earlier held frame however many presentations later it is
    // copied, so it can never corroborate an observation of that same frame.
    const std::optional<std::uint64_t> pendingAcquisition = EvidenceAcquisition(pendingEvidenceStamp_);
    const bool independentPending = !immutable && pendingEvidenceStamp_.SameScene(stamp) &&
        pendingEvidenceAuthority_ == result.authority &&
        pendingEvidenceStamp_.frameSeq != stamp.frameSeq &&
        pendingAcquisition && *pendingAcquisition != *acquisition &&
        (liveCamera
            ? pendingEvidenceStamp_.liveCameraCapture && stamp.liveCameraCapture &&
                *acquisition > *pendingAcquisition
            : pendingDigestKind_ == result.sourcePixelDigestKind &&
                pendingEvidenceDigest_.size() == 32 && result.sourcePixelDigest.size() == 32 &&
                pendingEvidenceDigest_ != result.sourcePixelDigest);
    // While a hold lasts no second acquisition can arrive. A single held
    // observation is offered as reading text only: the original ink stays and
    // no glyph, screen-reader word or proof of agreement is claimed for it.
    const bool heldReadingOnly = !immutable && stamp.holdDisplay == HoldDisplay::Held;
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
        const auto index = static_cast<qsizetype>(candidate.source - prepared.constData());
        if (independentNeighbor && candidate.line.reference.valid)
        {
            if (workerRegistration) neighborRegistered = corroboration->corroborationMatches[index];
            else if (matchTimer.nsecsElapsed() / 1e6 < tuning_.admitMatchBudgetMs)
                neighborRegistered = MatchPatch(candidate.line.reference, corroboration->reference, tuning_.match).matched;
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
                const auto wordIndex = static_cast<qsizetype>(&word - candidate.source->words.constData());
                const bool registeredWord = independentNeighbor && candidate.line.reference.valid &&
                    workerWordRegistration && evidence != neighborWords.constEnd() &&
                    corroboration->corroborationWordMatches[index][wordIndex];
                const bool glyphEligible = !immutable || registeredWord || (neighborRegistered && evidence != neighborWords.constEnd() &&
                    std::any_of(evidence->begin(), evidence->end(), [&](const QRectF& r) {
                        return Iou(word.sceneRect, r) >= tuning_.agreementIou;
                    }));
                candidate.line.words.push_back({word.text, word.sceneRect, glyphEligible,
                    word.colors ? std::optional<LineColors>(StableLineColors(*word.colors)) : std::nullopt});
                candidate.line.readingTokens.push_back(word.text);
                if (!glyphEligible) candidate.line.protectedSceneRects.push_back(
                    word.protectedSceneRect.isEmpty() ? word.sceneRect : word.protectedSceneRect);
            } else if (heldReadingOnly) {
                candidate.line.words.push_back({word.text, word.sceneRect, false});
                candidate.line.readingTokens.push_back(word.text);
                candidate.line.protectedSceneRects.push_back(
                    word.protectedSceneRect.isEmpty() ? word.sceneRect : word.protectedSceneRect);
                nextPending.push_back({word.text, currentRect, current.sourceEpoch,
                                       current.geometryGeneration});
            } else {
                ++candidate.line.omittedWords;
                candidate.line.readingTokens.push_back({});
                candidate.line.protectedSceneRects.push_back(
                    word.protectedSceneRect.isEmpty() ? word.sceneRect : word.protectedSceneRect);
                nextPending.push_back({word.text, currentRect, current.sourceEpoch,
                                       current.geometryGeneration});
            }
        }
        if ((immutable || heldReadingOnly) && candidate.line.reference.valid &&
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
                        for (int i = 0; i < old.words.size(); ++i)
                            if (candidate.line.words[i].colors && old.words[i].colors)
                                candidate.line.words[i].colors = StableLineColors(
                                    *candidate.line.words[i].colors, &*old.words[i].colors);
                        if (!IsShown(old, nowMs, latestProbe->stamp.frameSeq) ||
                            (candidate.line.valid &&
                             (candidate.line.validatedAtMs > old.validatedAtMs ||
                              candidate.line.validatedFrameSeq > old.validatedFrameSeq))) break;
                        if (retainedIds.contains(old.id)) {
                            duplicate = true;
                            break;
                        }
                        const LineColors colors = candidate.line.colors;
                        auto measuredWords = std::move(candidate.line.words);
                        candidate.line = old;
                        candidate.line.colors = colors;
                        for (int i = 0; i < candidate.line.words.size(); ++i)
                            candidate.line.words[i].colors = measuredWords[i].colors;
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
        const QRectF oldRect = line.sceneRect.translated(line.sceneOffset);
        if (!immutable && latestProbe && latestProbe->stamp.SameScene(current) &&
            latestProbe->stamp.frameSeq >= stamp.frameSeq &&
            line.validatedFrameSeq >= stamp.frameSeq &&
            IsShown(line, nowMs, latestProbe->stamp.frameSeq) &&
            std::any_of(failedRegistrationRects.begin(), failedRegistrationRects.end(),
                [&](const QRectF& r) { return r.intersects(oldRect); }) &&
            std::none_of(admitted.begin(), admitted.end(), [&](const AcceptedLine& replacement) {
                return replacement.sceneRect.translated(replacement.sceneOffset).intersects(oldRect);
            })) {
            kept.push_back(line); // exact old identity/reference/proof, never refreshed by failed OCR
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
    const auto glyphWord = [](const AcceptedWord& word) { return word.glyphEligible; };
    if (std::any_of(admitted.begin(), admitted.end(), [&](const AcceptedLine& line) {
        return !immutable ? std::any_of(line.words.begin(), line.words.end(), glyphWord)
                          : line.reference.valid && std::any_of(line.words.begin(), line.words.end(), glyphWord);
    })) {
        return AdmitVerdict::kAccepted;
    }
    if ((immutable || heldReadingOnly) && !admitted.isEmpty()) return AdmitVerdict::kPending;
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

ValidationSummary OverlayModel::ApplyChecks(const QVector<LineCheck>& checks, const SourceStamp& probeStamp,
                                            bool renew)
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
        if (!renew && probeStamp.frameSeq >= line.provenance.frameSeq &&
            probeStamp.timestampMs >= line.validatedAtMs) {
            // Composite identity alone revokes display, even if no line check
            // was queued or the worker exhausted its budget. It never alters
            // the last single-frame proof, allowing later live revalidation.
            line.valid = false;
        }
        const LineCheck* check = byId.value(line.id, nullptr);
        // Not part of this probe (admitted after it was taken), or already
        // validated against newer pixels: keep the newer state.
        if (!check || probeStamp.timestampMs < line.validatedAtMs) {
            kept.push_back(line);
            continue;
        }
        if (!line.valid && line.provenance.temporallySmoothed &&
            probeStamp.frameSeq < line.provenance.frameSeq) {
            // An older single-frame probe cannot validate smoothed text
            // recognized from a newer blend; keep it unverified.
            ++summary.unchecked;
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
        if (!renew) {
            // A composite agreeing with the line is not proof of either source:
            // revoke its display (glyphs and reading text) until a single-frame
            // probe revalidates it, without touching its proof clocks or offset.
            line.valid = false;
            kept.push_back(line);
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
    // Same authority as the worker path: a composite probe may hide or
    // remove text but never renews its proof.
    return ApplyChecks(CheckLines(LineReferences(probe.stamp), probe, tuning_.match, 0.0), probe.stamp,
                       EvidenceAcquisition(probe.stamp).has_value());
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
    // Conflict ink is shared scene protection even when its entire former
    // line was removed. Adjacent plates must never occlude those originals.
    QVector<QRectF> protectedRects = immutable_ ? immutableConflictRects_ : QVector<QRectF>{};
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
            words.push_back({word.text, word.sceneRect.translated(line.sceneOffset), word.colors.value_or(line.colors), line.id,
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
        const bool indexed = !line->readingWordIndices.isEmpty();
        if (indexed && line->readingWordIndices.size() != line->readingTokens.size()) continue;
        QSet<int> named;
        for (qsizetype t = 0; t < line->readingTokens.size(); ++t) {
            const QString& token = line->readingTokens[t];
            const int index = indexed ? line->readingWordIndices[t] : next;
            if (token.isEmpty()) {
                if (indexed && index >= 0) { consistent = false; break; }
                raw.push_back(-1);
                continue;
            }
            if (index < 0 || index >= line->words.size() || line->words[index].text != token ||
                named.contains(index)) {
                consistent = false;
                break;
            }
            named.insert(index);
            raw.push_back(line->words[index].glyphEligible ? index : -1);
            ++next;
        }
        if (!consistent || (!indexed && next != line->words.size())) continue;

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
        if (line.readingWordIndices.isEmpty()) count += line.words.size();
        else {
            QSet<int> named;
            for (const int index : line.readingWordIndices)
                if (index >= 0 && index < line.words.size()) named.insert(index);
            count += named.size();
        }
    }
    return count;
}

bool OverlayModel::HasAcceptedText() const
{
    return AcceptedWordCount() > 0;
}

} // namespace okuflow::uct

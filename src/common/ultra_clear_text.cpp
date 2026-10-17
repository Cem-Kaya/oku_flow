#include "okuflow/common/ultra_clear_text.hpp"

#include <QElapsedTimer>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
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
    if (x0 < 0.0 || y0 < 0.0 || x1 > probe.luma.width() || y1 > probe.luma.height()) {
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
    return t.destinationWidth * probe.luma.width() / t.sourceWidth;
}

double PixelsPerSceneY(const ProbeFrame& probe)
{
    const ViewTransform& t = probe.stamp.viewTransform;
    return t.destinationHeight * probe.luma.height() / t.sourceHeight;
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
    // Printed glyphs cover the minority of a line box.
    const bool darkText = darkCount <= lightCount;
    colors.foreground = darkText ? darkColor : lightColor;
    colors.background = darkText ? lightColor : darkColor;
    if (!colors.foreground.isValid() || !colors.background.isValid()) {
        colors = {};
    }
    return colors;
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

} // namespace

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
    // At most 2x2 samples per block keep the cost proportional to the probe
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

ReferencePatch BuildReferencePatch(const ProbeFrame& reference, const QRectF& lineRect)
{
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

PatchMatch MatchPatch(const ReferencePatch& patch, const ProbeFrame& probe, const MatchTuning& tuning)
{
    PatchMatch match;
    if (!patch.valid || !probe.IsValid()) {
        return match;
    }
    const PatchGeometry g = ProjectPatch(patch, probe);
    const Correlation zero = Correlate(patch, g, probe, 0.0, 0.0, 1);
    match.visible = zero.visibleFraction >= tuning.minimumVisibleFraction;
    if (!match.visible) {
        return match;
    }
    double bestScore = zero.score;
    double bestX = 0.0, bestY = 0.0;
    // Static fast path: an unmoved, unchanged line is the common case.
    if (zero.score < tuning.minimumScore + 0.08) {
        const int radius = std::max(1, tuning.searchRadiusPx);
        for (int dy = -radius; dy <= radius; dy += 2) {
            for (int dx = -radius; dx <= radius; dx += 2) {
                const Correlation c = Correlate(patch, g, probe, dx, dy, 3);
                if (c.visibleFraction >= tuning.minimumVisibleFraction && c.score > bestScore) {
                    bestScore = c.score;
                    bestX = dx;
                    bestY = dy;
                }
            }
        }
        const double coarseX = bestX, coarseY = bestY;
        bestScore = Correlate(patch, g, probe, coarseX, coarseY, 1).score;
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dy == 0) {
                    continue;
                }
                const Correlation c =
                    Correlate(patch, g, probe, coarseX + dx, coarseY + dy, 1);
                if (c.visibleFraction >= tuning.minimumVisibleFraction && c.score > bestScore) {
                    bestScore = c.score;
                    bestX = coarseX + dx;
                    bestY = coarseY + dy;
                }
            }
        }
        // Sub-pixel parabolic refinement on each axis.
        const auto refine = [&](bool horizontal) {
            const double sx = horizontal ? 1.0 : 0.0;
            const double sy = horizontal ? 0.0 : 1.0;
            const double minus = Correlate(patch, g, probe, bestX - sx, bestY - sy, 1).score;
            const double plus = Correlate(patch, g, probe, bestX + sx, bestY + sy, 1).score;
            const double denominator = minus - 2.0 * bestScore + plus;
            if (denominator < -1e-9) {
                return std::clamp(0.5 * (minus - plus) / denominator, -0.5, 0.5);
            }
            return 0.0;
        };
        const double subX = refine(true);
        const double subY = refine(false);
        bestX += subX;
        bestY += subY;
    }
    match.score = bestScore;
    match.matched = bestScore >= tuning.minimumScore;
    match.sceneOffset = {bestX * g.sceneUnitsPerPixelX, bestY * g.sceneUnitsPerPixelY};
    return match;
}

std::optional<double> SceneSimilarity(const ProbeFrame& a, const ProbeFrame& b)
{
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

    static void Run(std::shared_ptr<State> self)
    {
        BackendStatus initial;
        if (self->recognizer) {
            initial = self->recognizer->Initialize();
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
            const BackendState state = [&] {
                std::lock_guard lock(self->mutex);
                return self->status.state;
            }();
            if (state != BackendState::kReady) {
                result.output.error = QStringLiteral("Recognition backend is not ready.");
            } else {
                result.output = self->recognizer->Recognize(job.pixels, self->cancelInFlight);
            }
            if (result.output.ok && !self->cancelInFlight.load()) {
                result.reference = MakeProbeFrame(job.pixels.constBits(), job.pixels.width(),
                                                  job.pixels.height(),
                                                  static_cast<int>(job.pixels.bytesPerLine()),
                                                  self->referenceDecimation, job.stamp);
                for (const RecognizedLine& line : result.output.lines) {
                    QRectF bounds;
                    for (const RecognizedWord& word : line.words) {
                        bounds = bounds.isNull() ? word.boundsPx : bounds.united(word.boundsPx);
                    }
                    result.lineColors.push_back(MeasureLineColors(job.pixels, bounds.toAlignedRect()));
                }
            }
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
                                           int referenceDecimation)
    : state_(std::make_shared<State>())
{
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
    std::lock_guard lock(state_->mutex);
    if (state_->shutdown || state_->status.state == BackendState::kUnavailable ||
        state_->status.state == BackendState::kFailed) {
        return 0;
    }
    if (state_->pending) {
        ++state_->stats.coalesced;
    }
    State::Job job;
    job.id = state_->nextJobId++;
    job.authority = state_->authority;
    job.stamp = stamp;
    job.pixels = std::move(owned);
    const std::uint64_t id = job.id;
    state_->pending = std::move(job);
    ++state_->stats.submitted;
    state_->wake.notify_one();
    return id;
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

// --- Overlay model -----------------------------------------------------------

OverlayModel::OverlayModel(ModelTuning tuning)
    : tuning_(tuning)
{
}

void OverlayModel::Reset()
{
    lines_.clear();
    pending_.clear();
}

AdmitVerdict OverlayModel::Admit(const RecognitionResult& result, const SourceStamp& current,
                                 const ProbeFrame* latestProbe, std::int64_t nowMs)
{
    if (!result.output.ok || result.output.cancelled) {
        return AdmitVerdict::kFailed;
    }
    if (!result.stamp.SameScene(current)) {
        return AdmitVerdict::kStaleScene;
    }
    if (nowMs - result.stamp.timestampMs > tuning_.maxResultAgeMs) {
        return AdmitVerdict::kTooOld;
    }
    if (!result.reference.IsValid()) {
        return AdmitVerdict::kFailed;
    }
    const SourceStamp& stamp = result.stamp;
    const double jobWidth = stamp.viewportWidth;
    const double jobHeight = stamp.viewportHeight;
    const bool newerProbe = latestProbe && latestProbe->IsValid() &&
                            latestProbe->stamp.SameScene(current) &&
                            latestProbe->stamp.timestampMs > stamp.timestampMs;

    struct Candidate {
        AcceptedLine line;
        QVector<AcceptedWord> words; // all usable words, job scene coords
    };
    QVector<Candidate> candidates;
    int movedLines = 0;
    int trackableLines = 0;
    for (int i = 0; i < result.output.lines.size(); ++i) {
        Candidate candidate;
        QRectF lineRect;
        for (const RecognizedWord& word : result.output.lines[i].words) {
            const QString text = word.text.trimmed();
            if (text.isEmpty() || word.boundsPx.height() < tuning_.minimumWordHeightPx) {
                continue;
            }
            const QRectF scene = ViewRectToScene(stamp.viewTransform, jobWidth, jobHeight, word.boundsPx);
            if (scene.isEmpty()) {
                continue;
            }
            candidate.words.push_back({text, scene});
            lineRect = lineRect.isNull() ? scene : lineRect.united(scene);
        }
        if (candidate.words.isEmpty()) {
            continue;
        }
        candidate.line.sceneRect = lineRect;
        candidate.line.reference = BuildReferencePatch(result.reference, lineRect);
        if (!candidate.line.reference.valid) {
            continue; // untrackable: never displayed
        }
        ++trackableLines;
        candidate.line.colors = i < result.lineColors.size() ? result.lineColors[i] : LineColors{};
        candidate.line.provenance = stamp;
        candidate.line.valid = true;
        candidate.line.validatedAtMs = stamp.timestampMs;
        candidate.line.validatedFrameSeq = stamp.frameSeq;
        if (newerProbe) {
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
        candidates.push_back(std::move(candidate));
    }
    if (trackableLines == 0) {
        return AdmitVerdict::kEmpty;
    }
    if (candidates.isEmpty() && movedLines > 0) {
        return AdmitVerdict::kContentMoved;
    }

    // Temporal agreement: a word is shown only after two independent
    // recognitions of the same scene agree on its text and position.
    const auto confirmedBefore = [&](const QString& text, const QRectF& currentRect) {
        for (const AcceptedLine& line : lines_) {
            if (!line.provenance.SameScene(current)) {
                continue;
            }
            for (const AcceptedWord& word : line.words) {
                if (word.text == text &&
                    Iou(word.sceneRect.translated(line.sceneOffset), currentRect) >= tuning_.agreementIou) {
                    return true;
                }
            }
        }
        for (const PendingWord& word : pending_) {
            if (word.sourceEpoch == current.sourceEpoch &&
                word.geometryGeneration == current.geometryGeneration && word.text == text &&
                Iou(word.sceneRect, currentRect) >= tuning_.agreementIou) {
                return true;
            }
        }
        return false;
    };

    QVector<AcceptedLine> admitted;
    QVector<PendingWord> nextPending;
    for (Candidate& candidate : candidates) {
        for (const AcceptedWord& word : candidate.words) {
            const QRectF currentRect = word.sceneRect.translated(candidate.line.sceneOffset);
            if (IsHighRiskToken(word.text)) {
                ++candidate.line.omittedWords;
                continue;
            }
            if (confirmedBefore(word.text, currentRect)) {
                candidate.line.words.push_back(word);
            } else {
                ++candidate.line.omittedWords;
                nextPending.push_back({word.text, currentRect, current.sourceEpoch,
                                       current.geometryGeneration});
            }
        }
        if (!candidate.line.words.isEmpty()) {
            candidate.line.id = nextLineId_++;
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
            kept.push_back(line);
        }
    }
    kept += admitted;
    lines_ = std::move(kept);
    pending_ = std::move(nextPending);
    if (!admitted.isEmpty()) {
        return AdmitVerdict::kAccepted;
    }
    return pending_.isEmpty() ? (movedLines > 0 ? AdmitVerdict::kContentMoved : AdmitVerdict::kEmpty)
                              : AdmitVerdict::kPending;
}

ValidationSummary OverlayModel::Validate(const ProbeFrame& probe, std::int64_t nowMs)
{
    Q_UNUSED(nowMs);
    ValidationSummary summary;
    if (!probe.IsValid()) {
        return summary;
    }
    constexpr int kMaximumCheckedLines = 48;
    QVector<AcceptedLine> kept;
    kept.reserve(lines_.size());
    for (AcceptedLine& line : lines_) {
        if (!line.provenance.SameScene(probe.stamp)) {
            ++summary.invalid;
            continue;
        }
        if (summary.checked >= kMaximumCheckedLines) {
            line.valid = false;
            kept.push_back(line);
            continue;
        }
        ++summary.checked;
        const PatchMatch m = MatchPatch(line.reference, probe, tuning_.match);
        if (!m.visible) {
            line.valid = false;
            ++summary.notVisible;
            kept.push_back(line);
            continue;
        }
        if (!m.matched) {
            ++summary.invalid; // content changed, occluded or moved too far
            continue;
        }
        line.valid = true;
        line.sceneOffset = m.sceneOffset;
        line.validatedAtMs = probe.stamp.timestampMs;
        line.validatedFrameSeq = probe.stamp.frameSeq;
        ++summary.valid;
        kept.push_back(line);
    }
    lines_ = std::move(kept);
    return summary;
}

QVector<SceneWord> OverlayModel::SceneWords(std::int64_t nowMs, std::uint64_t currentFrameSeq) const
{
    QVector<SceneWord> words;
    for (const AcceptedLine& line : lines_) {
        if (!line.valid) {
            continue;
        }
        const bool fresh = nowMs - line.validatedAtMs <= tuning_.freshnessMs ||
                           currentFrameSeq == line.validatedFrameSeq;
        if (!fresh) {
            continue;
        }
        for (const AcceptedWord& word : line.words) {
            words.push_back({word.text, word.sceneRect.translated(line.sceneOffset), line.colors, line.id});
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

QString OverlayModel::ReadingText() const
{
    QVector<const AcceptedLine*> ordered;
    for (const AcceptedLine& line : lines_) {
        ordered.push_back(&line);
    }
    std::sort(ordered.begin(), ordered.end(), [](const AcceptedLine* a, const AcceptedLine* b) {
        const QRectF ra = a->sceneRect.translated(a->sceneOffset);
        const QRectF rb = b->sceneRect.translated(b->sceneOffset);
        const double band = std::min(ra.height(), rb.height()) * 0.5;
        if (std::abs(ra.center().y() - rb.center().y()) > band) {
            return ra.center().y() < rb.center().y();
        }
        return ra.left() < rb.left();
    });
    QStringList out;
    for (const AcceptedLine* line : ordered) {
        QStringList words;
        for (const AcceptedWord& word : line->words) {
            words.push_back(word.text);
        }
        QString text = words.join(QLatin1Char(' '));
        if (line->omittedWords > 0) {
            text += QStringLiteral(" …");
        }
        out.push_back(text);
    }
    return out.join(QLatin1Char('\n'));
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

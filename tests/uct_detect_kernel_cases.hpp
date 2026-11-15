#pragma once
// Detector post-processing: the default kernels must reproduce the original
// kernels bit for bit. Included after GpuBuffer/FixtureStream are defined.
#include <QPainter>
#include <QPainterPath>
#include <cstring>
#include <random>

#if OKUFLOW_ENABLE_CUDA
namespace uct_detect_test
{
// Probability maps chosen to stress what the three kernels changed: long
// union-find chains (spirals, combs, one huge blob), many roots per block,
// large and tilted bounding scans around the .4 mean threshold, curved and
// straight ribbons, more than 128 components and non-finite values.
inline std::vector<float> Map(int w, int h, int variant)
{
    QImage image(w, h, QImage::Format_Grayscale8);
    image.fill(Qt::black);
    QPainter painter(&image);
    std::mt19937 random(20261115u + unsigned(variant) * 7919u + unsigned(w) * 31u + unsigned(h));
    const auto real = [&](double low, double high) { return std::uniform_real_distribution<double>(low, high)(random); };
    const auto gray = [&](int low, int high) {
        const int value = std::uniform_int_distribution<int>(low, high)(random);
        return QColor(value, value, value);
    };
    switch (variant % 9)
    {
    case 0: // rows of short words, well above 128 components
        for (int y = 6; y + 8 < h; y += 14)
            for (int x = 4; x + 30 < w; x += 36)
                painter.fillRect(QRectF(x + real(0, 3), y + real(0, 2), real(12, 30), real(4, 9)), gray(70, 255));
        break;
    case 1: { // one spiral: the longest parent chains
        painter.setPen(QPen(gray(150, 255), 3));
        QPainterPath path(QPointF(w / 2.0, h / 2.0));
        for (int i = 1; i < 900; ++i) {
            const double angle = i * .09, radius = i * std::min(w, h) / 2000.0;
            path.lineTo(w / 2.0 + radius * std::cos(angle) * 1.6, h / 2.0 + radius * std::sin(angle));
        }
        painter.drawPath(path);
        break;
    }
    case 2: // tilted long lines with different strengths
        painter.setRenderHint(QPainter::Antialiasing);
        for (int i = 0; i < 26; ++i) {
            painter.save();
            painter.translate(real(w * .2, w * .8), real(h * .1, h * .9));
            painter.rotate(real(-35, 35));
            painter.fillRect(QRectF(-real(60, w * .35), -real(3, 9), real(120, w * .7), real(6, 18)), gray(60, 255));
            painter.restore();
        }
        break;
    case 3: // one blob covering most of the map, with holes
        painter.fillRect(QRect(3, 3, w - 6, h - 6), gray(110, 200));
        for (int i = 0; i < 40; ++i)
            painter.fillRect(QRectF(real(0, w), real(0, h), real(4, 40), real(4, 40)), Qt::black);
        break;
    case 4: // comb: every tooth joins late through the spine
        for (int x = 2; x + 2 < w; x += 4)
            painter.fillRect(QRect(x, 2, 2, h - 6), gray(120, 255));
        painter.fillRect(QRect(2, h - 5, w - 4, 3), gray(120, 255));
        break;
    case 5: // curved bands for the ribbon fit, drawn after the painter is released
        break;
    case 6: // small rotated boxes near the .4 mean: the inclusion test decides eligibility
        painter.setRenderHint(QPainter::Antialiasing);
        for (int i = 0; i < 1500; ++i) {
            painter.save();
            painter.translate(real(4, w - 4), real(4, h - 4));
            painter.rotate(real(-80, 80));
            painter.fillRect(QRectF(-real(2, 9), -real(1, 4), real(4, 18), real(2, 8)), gray(85, 135));
            painter.restore();
        }
        break;
    case 7: // faint wide bands whose mean sits on the threshold
        for (int y = 8; y + 12 < h; y += 18)
            painter.fillRect(QRectF(real(4, 40), y, real(w * .3, w * .9), real(5, 11)), gray(96, 110));
        break;
    default: // sparse specks and small boxes: many blocks with several roots
        for (int i = 0; i < 2600; ++i)
            painter.fillRect(QRectF(real(0, w - 5), real(0, h - 5), real(1, 6), real(1, 6)), gray(40, 255));
        break;
    }
    painter.end();
    if (variant % 9 == 5) {
        // Hard-edged interior parabolic bands, like the existing ribbon fixture:
        // enough sagitta for a curved fit, with alternating bend directions.
        const int bands = std::max(1, h / 56), left = w / 8, right = w - w / 8;
        for (int band = 0; band < bands; ++band) {
            const double half = (right - left) / 2.0, middle = (left + right) / 2.0;
            const double sag = real(7, 13) * (band % 2 ? -1 : 1), thickness = real(6, 9);
            const double base = (band + .5) * h / bands - sag / 2;
            const uchar level = uchar(std::uniform_int_distribution<int>(150, 245)(random));
            for (int x = left; x <= right; ++x) {
                const double center = base + sag * (x - middle) * (x - middle) / (half * half);
                for (int y = std::max(0, int(std::ceil(center - thickness))); y <= std::min(h - 1, int(std::floor(center + thickness))); ++y)
                    image.scanLine(y)[x] = level;
            }
        }
    }
    std::vector<float> map(std::size_t(w) * h);
    std::uniform_real_distribution<float> jitter(-.03f, .03f);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float value = image.constScanLine(y)[x] / 255.f;
            map[std::size_t(y) * w + x] = value > 0 ? std::clamp(value + jitter(random), 0.f, 1.f) : 0.f;
        }
    if (variant % 2) {
        // A detector's background is tiny but not zero: long runs of very
        // small values keep a box sum in low binades before any text is added.
        std::uniform_real_distribution<float> exponent(-9.f, -3.f);
        for (auto& value : map) if (value == 0.f) value = std::pow(10.f, exponent(random));
    }
    if (variant >= 9 && variant < 18) {
        // Quantized probabilities make exact halves common once a box sum
        // reaches the matching binade, so ties and their parity rule decide bits.
        const float quantum = variant % 2 ? 1.f / 8192 : 1.f / 1024;
        for (auto& value : map) value = std::round(value / quantum) * quantum;
    }
    if (variant >= 18 && variant % 3 == 0) {
        // Values the fast sum must hand to the serial fold: negative, above
        // four and subnormal samples inside otherwise ordinary boxes.
        std::uniform_int_distribution<std::size_t> at(0, map.size() - 1);
        for (int i = 0; i < 300; ++i)
            map[at(random)] = i % 3 == 0 ? -.75f : i % 3 == 1 ? 6.5f : 1e-41f;
    }
    if (variant >= 18) { // the same shapes with non-finite samples, which become background
        std::uniform_int_distribution<std::size_t> at(0, map.size() - 1);
        for (int i = 0; i < 200; ++i)
            map[at(random)] = i % 3 == 0 ? std::numeric_limits<float>::quiet_NaN()
                : i % 3 == 1 ? std::numeric_limits<float>::infinity() : -std::numeric_limits<float>::infinity();
    }
    return map;
}

struct Output {
    int count{};
    std::vector<okuflow::uct::gpu::Quad> quads;
    std::vector<int> labels;
    std::vector<okuflow::uct::gpu::Component> components;
};

inline bool SameBits(const void* a, const void* b, std::size_t bytes) { return std::memcmp(a, b, bytes) == 0; }

// Field by field: padding inside Quad/Geometry/Component is not part of the contract.
inline QString Difference(const Output& a, const Output& b, bool ribbons)
{
    using namespace okuflow::uct;
    if (a.count != b.count) return QStringLiteral("count %1 != %2").arg(a.count).arg(b.count);
    for (int i = 0; i < std::min(a.count, 128); ++i) {
        const auto &p = a.quads[std::size_t(i)], &q = b.quads[std::size_t(i)];
        if (!SameBits(p.xy, q.xy, sizeof(p.xy)) || !SameBits(&p.score, &q.score, sizeof(float)) ||
            p.validWidth != q.validWidth || p.internalEdges != q.internalEdges || p.componentRoot != q.componentRoot)
            return QStringLiteral("quad %1 differs").arg(i);
        const auto &r = p.ribbon, &s = q.ribbon;
        const double left[]{r.ox, r.oy, r.ux, r.uy, r.vx, r.vy, r.a, r.b, r.c, r.h0, r.h1, r.start, r.end,
                            r.protectLeft, r.protectTop, r.protectRight, r.protectBottom};
        const double right[]{s.ox, s.oy, s.ux, s.uy, s.vx, s.vy, s.a, s.b, s.c, s.h0, s.h1, s.start, s.end,
                             s.protectLeft, s.protectTop, s.protectRight, s.protectBottom};
        if (r.valid != s.valid || r.unsupported != s.unsupported || !SameBits(left, right, sizeof(left)))
            return QStringLiteral("ribbon %1 differs").arg(i);
    }
    if (!ribbons) return {}; // without ribbon scratch the labels become block offsets, compared through quads
    if (a.labels != b.labels) return QStringLiteral("labels differ");
    for (std::size_t i = 0; i < a.components.size(); ++i) {
        const auto &c = a.components[i], &d = b.components[i];
        const float left[]{c.c, c.s, c.u0, c.u1, c.v0, c.v1, c.score}, right[]{d.c, d.s, d.u0, d.u1, d.v0, d.v1, d.score};
        if (c.n != d.n || c.x != d.x || c.y != d.y || c.xx != d.xx || c.yy != d.yy || c.xy != d.xy ||
            !SameBits(left, right, sizeof(left)))
            return QStringLiteral("component %1 differs").arg(i);
    }
    return {};
}

inline void KernelsMatchReference()
{
    using namespace okuflow::uct;
    int devices{};
    if (cudaGetDeviceCount(&devices) != cudaSuccess || devices < 1) QSKIP("CUDA device unavailable.");
    FixtureStream stream;
    int ribbonFits = 0, ribbonRefusals = 0, eligible = 0, largest = 0;
    QStringList failures;
    bool every = true; // the first size runs every variant, the others the nine clean shapes
    for (const QSize size : {QSize(960, 544), QSize(928, 960), QSize(333, 257), QSize(96, 64)}) {
        const int w = size.width(), h = size.height(), pixels = w * h;
        GpuBuffer<float> map(pixels);
        GpuBuffer<int> labels(pixels), count(1), offsets((pixels + 255) / 256);
        GpuBuffer<gpu::Component> components(pixels);
        GpuBuffer<gpu::Quad> quads(128);
        const auto run = [&](const std::vector<float>& values, bool ribbons, gpu::DetectKernels kernels, Output& output) {
            output.quads.assign(128, {});
            output.labels.assign(std::size_t(pixels), 0);
            output.components.assign(std::size_t(pixels), {});
            if (cudaMemcpyAsync(map.data, values.data(), values.size() * sizeof(float), cudaMemcpyHostToDevice, stream.value) != cudaSuccess ||
                cudaMemsetAsync(quads.data, 0, 128 * sizeof(gpu::Quad), stream.value) != cudaSuccess) return false;
            gpu::Detect(map.data, w, h, w * 2, h * 2, labels.data, components.data, quads.data, count.data,
                        stream.value, ribbons ? offsets.data : nullptr, kernels);
            return cudaGetLastError() == cudaSuccess &&
                cudaMemcpyAsync(&output.count, count.data, sizeof(int), cudaMemcpyDeviceToHost, stream.value) == cudaSuccess &&
                cudaMemcpyAsync(output.quads.data(), quads.data, 128 * sizeof(gpu::Quad), cudaMemcpyDeviceToHost, stream.value) == cudaSuccess &&
                cudaMemcpyAsync(output.labels.data(), labels.data, std::size_t(pixels) * sizeof(int), cudaMemcpyDeviceToHost, stream.value) == cudaSuccess &&
                cudaMemcpyAsync(output.components.data(), components.data, std::size_t(pixels) * sizeof(gpu::Component), cudaMemcpyDeviceToHost, stream.value) == cudaSuccess &&
                cudaStreamSynchronize(stream.value) == cudaSuccess;
        };
        for (int variant = 0; variant < (every ? 27 : 9); ++variant) {
            const auto values = Map(w, h, variant);
            for (const bool ribbons : {false, true}) {
                Output reference;
                QVERIFY(run(values, ribbons, gpu::DetectKernels::Reference(), reference));
                eligible += reference.count;
                largest = std::max(largest, reference.count);
                for (int i = 0; i < std::min(reference.count, 128); ++i) {
                    ribbonFits += reference.quads[std::size_t(i)].ribbon.valid;
                    ribbonRefusals += reference.quads[std::size_t(i)].ribbon.unsupported;
                }
                const gpu::DetectKernels candidates[]{{}, {true, false, false, false}, {false, true, false, false},
                                                      {false, false, true, false}};
                for (const auto& kernels : candidates)
                    for (int repeat = 0; repeat < (&kernels == candidates ? 2 : 1); ++repeat) { // arrival order must not matter
                        Output actual;
                        QVERIFY(run(values, ribbons, kernels, actual));
                        const auto difference = Difference(reference, actual, ribbons);
                        if (!difference.isEmpty() && failures.size() < 12)
                            failures.push_back(QStringLiteral("%1x%2 variant %3 ribbons %4 kernels %5%6%7: %8")
                                .arg(w).arg(h).arg(variant).arg(ribbons).arg(kernels.compressRoots).arg(kernels.cooperativeBoxes)
                                .arg(kernels.parallelRibbonMerge).arg(difference));
                    }
            }
        }
        every = false;
    }
    QVERIFY2(failures.isEmpty(), qPrintable(failures.join(QStringLiteral("; "))));
    // The comparison is vacuous unless the maps reach the code that changed.
    QVERIFY2(eligible > 2000 && largest > 128 && ribbonFits >= 20,
             qPrintable(QStringLiteral("eligible %1 largest %2 ribbon fits %3 refusals %4")
                 .arg(eligible).arg(largest).arg(ribbonFits).arg(ribbonRefusals)));
    qInfo() << "detector kernel comparison: eligible" << eligible << "largest count" << largest
            << "valid ribbons" << ribbonFits << "refused ribbons" << ribbonRefusals;
}
} // namespace uct_detect_test
#endif

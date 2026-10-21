#pragma once

#include "uct_book_crop_test_support.hpp"
#include "../src/common/uct_color_policy.hpp"
#include <QCryptographicHash>

namespace uct_small_ink_test {
using namespace okuflow::uct;

struct Fixture {
    QString name;
    QImage image;
    RecognitionOutput output;
    QColor ink, paper;
    QRectF expectedInk;
    bool sourceCrop{};
    bool fallback{};
    bool gpuCapFallback{};
};

inline RecognizedWord Word(QString text, const QRectF& detected, const QRectF& search)
{
    RecognizedWord word;
    word.text = std::move(text);
    word.boundsPx = word.protectedBoundsPx = detected;
    word.quadPx = TextQuad::FromRect(detected);
    word.inkSearchPx = search;
    word.inkSearchQuadPx = TextQuad::FromRect(search);
    word.paddedBounds = true;
    return word;
}

inline QVector<Fixture> Fixtures()
{
    QVector<Fixture> fixtures;
    // Every subpixel sampling phase, both polarities, neutral/chromatic paper
    // and a gentle paper gradient. Opaque one-pixel stems are surrounded by AA;
    // neither a body median nor a hardcoded darkest pixel is the desired ink.
    for (int phaseY : {0, 1}) for (int phaseX : {0, 1})
        for (int surface : {0, 1, 2}) for (bool gradient : {false, true}) {
        Fixture f;
        f.name = QStringLiteral("one-pixel-stems-x%1-y%2-surface%3-gradient%4")
            .arg(phaseX).arg(phaseY).arg(surface).arg(gradient);
        f.ink = surface == 1 ? QColor(224, 230, 218) : QColor(65, 54, 79);
        f.paper = surface == 1 ? QColor(43, 52, 70)
            : surface == 2 ? QColor(235, 191, 93) : QColor(232, 227, 220);
        f.image = QImage(96, 64, QImage::Format_ARGB32);
        f.image.fill(f.paper);
        const QRect detected(30, 18, 15, 23);
        const QRect search = detected.adjusted(-3, -3, 6, 3);
        const auto put = [&](int x, int y, int alpha) {
            const QColor paper = f.image.pixelColor(x, y);
            f.image.setPixelColor(x, y, QColor(
                (f.ink.red() * alpha + paper.red() * (256 - alpha)) / 256,
                (f.ink.green() * alpha + paper.green() * (256 - alpha)) / 256,
                (f.ink.blue() * alpha + paper.blue() * (256 - alpha)) / 256));
        };
        if (gradient)
            for (int y = 0; y < f.image.height(); ++y)
                for (int x = 0; x < f.image.width(); ++x) {
                    const int delta = std::clamp((x - detected.x()) / 4 - 2, -4, 4);
                    f.image.setPixelColor(x, y, QColor(f.paper.red() + delta,
                        f.paper.green() + delta, f.paper.blue() + delta));
                }
        const int left = detected.x() + 3 + phaseX, top = detected.y() + 7 + phaseY;
        for (int x : {left, left + 4, left + 8})
            for (int y = top; y < top + 9; ++y) {
                put(x - 1, y, 128); put(x + 1, y, 128); put(x, y, 256);
            }
        // Detached dot and descending stem must survive fitting. These are
        // scripted pixels, not an OCR spelling or a line-color assumption.
        put(left, top - 4, 256); put(left, top - 3, 256);
        for (int y = top + 9; y < top + 12; ++y) put(left + 4, y, 256);
        // A darker/lighter isolated outlier must not replace robust glyph ink.
        f.image.setPixelColor(detected.x() + 1, detected.y(),
            surface == 1 ? QColor(Qt::white) : QColor(Qt::black));
        // A disconnected vertical rule is in search padding, beyond the
        // detector. It must not become the word's trailing letter.
        for (int y = search.top(); y <= search.bottom(); ++y)
            f.image.setPixelColor(search.right() - 1, y, f.ink);
        f.output.ok = true;
        RecognizedLine line;
        line.words.push_back(Word(QStringLiteral("jij"), detected, search));
        f.output.lines.push_back(line);
        f.expectedInk = QRectF(left - 1, top - 4, 11, 16);
        fixtures.push_back(std::move(f));
    }

    const auto base = fixtures.first();
    auto clipped = base;
    clipped.name = QStringLiteral("connected-stroke-crosses-detector-right");
    for (int y = 15; y < 44; ++y) clipped.image.setPixelColor(49,y,clipped.paper);
    for (int x = 41; x <= 47; ++x) clipped.image.setPixelColor(x,29,clipped.ink);
    clipped.expectedInk.setRight(48);
    fixtures.push_back(std::move(clipped));

    auto connectedLight = base;
    connectedLight.name = QStringLiteral("lighter-connected-clipped-stroke-retained");
    for (int y = 15; y < 44; ++y) connectedLight.image.setPixelColor(49,y,connectedLight.paper);
    for (int x = 41; x <= 47; ++x) connectedLight.image.setPixelColor(x,29,QColor(160,160,160));
    connectedLight.expectedInk.setRight(48);
    fixtures.push_back(std::move(connectedLight));

    auto weakGlyph = base;
    weakGlyph.name = QStringLiteral("weak-only-last-glyph-preserves-complete-detector");
    for (int y = 25; y < 34; ++y) weakGlyph.image.setPixelColor(41,y,QColor(160,160,160));
    weakGlyph.fallback = true;
    weakGlyph.expectedInk = weakGlyph.output.lines[0].words[0].boundsPx;
    fixtures.push_back(std::move(weakGlyph));

    auto detached = base;
    detached.name = QStringLiteral("detached-dot-above-detector");
    // Keep the original exact pixels/search but place its two-row dot in the
    // existing detached-mark allowance above the original detector.
    detached.output.lines[0].words[0] = Word(QStringLiteral("jij"), {30,23,15,18}, {27,15,24,29});
    fixtures.push_back(std::move(detached));

    auto adjacent = base;
    adjacent.name = QStringLiteral("adjacent-line-across-search-top");
    for (int y = 15; y <= 16; ++y) for (int x = 27; x <= 50; ++x)
        adjacent.image.setPixelColor(x,y,adjacent.ink);
    fixtures.push_back(std::move(adjacent));

    for (bool noiseOnly : {false,true}) {
        auto refused = base;
        refused.name = noiseOnly ? QStringLiteral("isolated-noise-refuses-fit")
            : QStringLiteral("two-pixel-rule-refuses-fit");
        refused.image.fill(refused.paper);
        if (noiseOnly) refused.image.setPixelColor(35,28,refused.ink);
        else for (int y = 28; y < 30; ++y) for (int x = 32; x < 43; ++x)
            refused.image.setPixelColor(x,y,refused.ink);
        refused.expectedInk = refused.output.lines[0].words[0].boundsPx;
        refused.fallback = true;
        fixtures.push_back(std::move(refused));
    }

    Fixture winding;
    winding.name = QStringLiteral("winding-upright-component-bounded-gpu-fallback");
    winding.image = QImage(160,160,QImage::Format_ARGB32);
    winding.image.fill(Qt::white);
    winding.ink=Qt::black; winding.paper=Qt::white;
    {
        QPainter painter(&winding.image); painter.setPen(QPen(Qt::black,1));
        painter.drawLine(14,10,14,14);
        for (int y=4,index=0; y<=124; y+=4,++index) {
            painter.drawLine(14,10+y,133,10+y);
            if (y<124) { const int x=index%2?14:133; painter.drawLine(x,10+y,x,14+y); }
        }
    }
    winding.output.ok=true;
    RecognizedLine windingLine;
    windingLine.words.push_back(Word(QStringLiteral("Winding"),{10,10,128,128},{10,10,128,128}));
    winding.output.lines.push_back(windingLine);
    winding.expectedInk={14,10,120,125}; winding.gpuCapFallback=true;
    fixtures.push_back(std::move(winding));

    Fixture book;
    book.name = QStringLiteral("exact-book-6x-to-an");
    book.image = uct_book_crop_test::Image();
    book.sourceCrop = true;
    book.output.ok = true;
    RecognizedLine line;
    // Coordinates are the original detector/protected and search boxes from
    // the saved 6x proof, translated only by the lossless crop origin (630,252).
    line.words.push_back(Word(QStringLiteral("to"),
        {10.6603773584906, 9.38201047789374, 12.075471698113212, 18.32127394766178},
        {9.1509433962265, 9.38201047789374, 15.09433962264151, 18.32127394766178}));
    line.words.push_back(Word(QStringLiteral("an"),
        {25.7547169811321, 9.29469450464785, 9.056603773584904, 18.29944423459611},
        {24.2452830188679, 9.29469450464785, 13.584905660377359, 18.29944423459611}));
    book.output.lines.push_back(line);
    fixtures.push_back(std::move(book));
    return fixtures;
}

inline std::optional<RecognitionResult> Prepare(const Fixture& f, SharedDeviceImage device = {})
{
    struct Backend final : Recognizer {
        RecognitionOutput output;
        bool resident{};
        BackendStatus Initialize() override { return {BackendState::kReady, "small-ink-fixture", "", ""}; }
        bool WantsDeviceInput() const override { return resident; }
        RecognitionOutput Recognize(const QImage&, const std::atomic<bool>&) override { return output; }
        RecognitionOutput RecognizeDevice(const DeviceImage&, const std::atomic<bool>&) override { return output; }
    };
    auto backend = std::make_unique<Backend>();
    backend->output = f.output; backend->resident = bool(device);
    SourceStamp stamp;
    stamp.sourceEpoch = stamp.geometryGeneration = stamp.frameSeq = 1;
    stamp.viewportWidth = f.image.width(); stamp.viewportHeight = f.image.height();
    stamp.viewTransform = {0,0,1,1,0,0,1,1,true};
    std::optional<RecognitionResult> result;
    std::atomic<bool> received{};
    RecognitionScheduler scheduler(std::move(backend), [&](auto r) {
        result = std::move(r); received.store(true);
    }, {});
    const bool submitted = device ? scheduler.SubmitDevice(stamp, device) : scheduler.Submit(stamp, f.image);
    if (!submitted) return {};
    QElapsedTimer wait; wait.start();
    while (!received.load() && wait.elapsed() < 5000) QTest::qWait(1);
    scheduler.Cancel();
    return result;
}

inline void Check(const Fixture& f, const RecognitionResult& result)
{
    QVERIFY2(result.output.ok, qPrintable(f.name + ": " + result.output.error));
    QCOMPARE(result.output.lines.size(), 1);
    QCOMPARE(result.output.lines[0].words.size(), f.output.lines[0].words.size());
    for (int i = 0; i < result.output.lines[0].words.size(); ++i) {
        const auto& actual = result.output.lines[0].words[i];
        const auto& original = f.output.lines[0].words[i];
        QVERIFY(actual.colors);
        QCOMPARE(actual.text, original.text);
        QCOMPARE(actual.disposition, TokenDisposition::kCandidate);
        QCOMPARE(actual.protectedBoundsPx, original.protectedBoundsPx);
        QCOMPARE(actual.inkSearchPx, original.inkSearchPx);
        const QString diagnostic = f.name + QStringLiteral(" word=%1 ink=%2 paper=%3 fit=%4 [%5,%6,%7,%8]")
            .arg(i).arg(actual.colors->foreground.name()).arg(actual.colors->background.name())
            .arg(actual.inkFitted).arg(actual.boundsPx.x()).arg(actual.boundsPx.y())
            .arg(actual.boundsPx.width()).arg(actual.boundsPx.height());
        qInfo().noquote() << diagnostic;
        if (f.sourceCrop) {
            // Actual source core lumas span 72--112. A representative fill may
            // vary within that range; the saved #bbb4ad halo (luma181) may not.
            const int luma = qGray(actual.colors->foreground.rgb());
            QVERIFY2(luma >= 65 && luma <= 120, qPrintable(diagnostic));
            QVERIFY2(uct_test::ColorDistance(actual.colors->background, QColor(232,227,220)) <= 4,
                qPrintable(diagnostic));
            if (!i) {
                QVERIFY2(actual.inkFitted, qPrintable(diagnostic));
                QVERIFY2(actual.boundsPx.height() >= 7 && actual.boundsPx.height() <= 9, qPrintable(diagnostic));
                QVERIFY2(actual.boundsPx.width() >= 7 && actual.boundsPx.width() <= 10, qPrintable(diagnostic));
                QVERIFY2(actual.boundsPx.left() >= 11 && actual.boundsPx.right() <= 21, qPrintable(diagnostic));
            } else {
                // Its actual six-pixel body does not clear the unchanged
                // 35% fit guard. Keep detector fallback; never pad it to 7px.
                QVERIFY2(!actual.inkFitted, qPrintable(diagnostic));
                QCOMPARE(actual.boundsPx, original.boundsPx);
            }
        } else {
            QCOMPARE(actual.inkFitted, !f.fallback);
            QCOMPARE(actual.colors->foreground, f.ink);
            QVERIFY2(uct_test::ColorDistance(actual.colors->background, f.paper) <= 4, qPrintable(diagnostic));
            QCOMPARE(actual.boundsPx, f.expectedInk);
        }
    }
    if (f.sourceCrop) {
        const auto& words = result.output.lines[0].words;
        QVERIFY2(words[1].boundsPx.left() - words[0].boundsPx.right() >= 2,
            "Distinct source glyphs need an actual fitted gap; OCR spelling is not pixel proof.");
        QCOMPARE(result.prepared.size(),1);
        QCOMPARE(result.prepared[0].words.size(),2);
        for (const auto& word : result.prepared[0].words)
            QCOMPARE(word.disposition,TokenDisposition::kCandidate);
    }
}

inline void Run(bool resident)
{
    // The budget boundary is a sampling-work decision, never a font-size cap.
    QCOMPARE(color_policy::BodySampleStep(64,64), 1);
    QCOMPARE(color_policy::BodySampleStep(1,4096), 1);
    QCOMPARE(color_policy::BodySampleStep(4096,1), 1);
    QCOMPARE(color_policy::BodySampleStep(65,64), 2);
    QCOMPARE(color_policy::BodySampleStep(1,4097), 2);
    QCOMPARE(color_policy::BodySampleStep(0,0), 2);
    QCOMPARE(color_policy::BodySampleStep(std::numeric_limits<int>::max(),
        std::numeric_limits<int>::max()), 2);
    for (const auto& f : Fixtures()) {
        QVERIFY2(!f.image.isNull(), qPrintable(f.name));
        if (f.sourceCrop) {
            QByteArray packed;
            for (int y=0; y<f.image.height(); ++y) for (int x=0; x<f.image.width(); ++x) {
                const auto color=f.image.pixelColor(x,y);
                packed.append(char(color.red())); packed.append(char(color.green())); packed.append(char(color.blue()));
            }
            QCOMPARE(QCryptographicHash::hash(packed,QCryptographicHash::Sha256).toHex(),
                QByteArray(uct_book_crop_test::kPackedRgbSha256));
        }
        const QImage original = f.image.copy();
        const auto cpu = Prepare(f);
        QVERIFY2(cpu.has_value(), qPrintable(f.name));
        Check(f, *cpu);
        QCOMPARE(f.image, original);
        if (!resident) continue;
#if OKUFLOW_ENABLE_CUDA
        struct Guard final : DeviceImage {
            SharedDeviceImage owned;
            mutable std::atomic<int> downloads{};
            explicit Guard(SharedDeviceImage p) : owned(std::move(p)) {
                width=owned->width; height=owned->height; pitch=owned->pitch; device=owned->device;
                sourceEpoch=owned->sourceEpoch; frameSequence=owned->frameSequence;
            }
            const void* Data() const override { return owned->Data(); }
            bool WaitReady(void* s, QString& e) const override { return owned->WaitReady(s,e); }
            bool HasResidentAnalysis() const override { return true; }
            bool PrepareResidentAnalysis(RecognitionResult& r, int d, bool p,
                const std::atomic<bool>& c, QString& e) const override {
                return owned->PrepareResidentAnalysis(r,d,p,c,e);
            }
            QImage Readback(const std::atomic<bool>&, QString&) const override { ++downloads; return {}; }
            QImage ReadProbe(const std::atomic<bool>&, QString&) const override { ++downloads; return {}; }
        };
        FixtureStream stream;
        GpuBuffer<unsigned char> pixels(f.image.sizeInBytes());
        AnalysisPool pool;
        QCOMPARE(cudaMemcpyAsync(pixels.data, f.image.constBits(), f.image.sizeInBytes(),
            cudaMemcpyHostToDevice, stream.value), cudaSuccess);
        auto captured = pool.Capture(pixels.data, f.image.bytesPerLine(), f.image.width(),
            f.image.height(), stream.value, 1, 1, 1);
        QVERIFY(captured);
        auto guarded = std::make_shared<Guard>(std::move(captured));
        const auto gpu = Prepare(f, guarded);
        QVERIFY2(gpu.has_value(), qPrintable(f.name));
        if (f.gpuCapFallback) {
            auto fallback=f; fallback.fallback=true;
            fallback.expectedInk=f.output.lines[0].words[0].boundsPx;
            Check(fallback,*gpu);
            qInfo() << "Upright winding component: complete detector fallback under fixed 64-pass GPU bound; CPU exact queue fits.";
        } else Check(f, *gpu);
        QCOMPARE(guarded->downloads.load(), 0);
        QVERIFY(gpu->reference.resident);
        for (int i = 0; i < cpu->output.lines[0].words.size(); ++i) {
            const auto& a = cpu->output.lines[0].words[i];
            const auto& b = gpu->output.lines[0].words[i];
            QCOMPARE(a.colors->foreground, b.colors->foreground);
            QCOMPARE(a.colors->background, b.colors->background);
            if (f.gpuCapFallback) {
                QVERIFY(a.inkFitted); QVERIFY(!b.inkFitted);
                QCOMPARE(b.boundsPx,f.output.lines[0].words[i].boundsPx);
                continue;
            }
            QCOMPARE(a.boundsPx, b.boundsPx);
            QCOMPARE(a.quadPx, b.quadPx);
            QCOMPARE(a.inkFitted, b.inkFitted);
        }
#endif
    }
}
} // namespace uct_small_ink_test

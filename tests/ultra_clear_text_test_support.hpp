#pragma once

// Deterministic synthetic printed-page generator and viewport renderer for
// Ultra Clear Text fixtures. No downloaded or private images are used.

#include "okuflow/common/ultra_clear_text.hpp"
#include "okuflow/common/view_transform.hpp"

#include <QFont>
#include <QCryptographicHash>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QStringList>

#include <cmath>
#include <algorithm>

namespace uct_test {

// A synthetic observation really changes a few non-text corner pixels.
// Temporal-admission fixtures must not manufacture independent evidence by
// changing only a timestamp, job number or claimed hash of identical pixels.
inline QImage IndependentFramePixels(const QImage& source, std::uint64_t frameSeq)
{
    QImage pixels = source.copy();
    if (pixels.isNull()) return pixels;
    for (int byte = 0; byte < std::min(8, pixels.width()); ++byte) {
        pixels.setPixelColor(pixels.width() - 1 - byte, pixels.height() - 1,
                             QColor(128, 128, static_cast<int>((frameSeq >> (byte * 8)) & 0xff)));
    }
    return pixels;
}

inline QByteArray SourcePixelsDigest(const QImage& pixels)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (int row = 0; row < pixels.height(); ++row)
        hash.addData(QByteArrayView(reinterpret_cast<const char*>(pixels.constScanLine(row)),
                                    static_cast<qsizetype>(pixels.width()) * 4));
    return hash.result();
}

struct PageWord {
    QString text;
    QRectF sceneRect; // normalized scene coordinates
};

struct SyntheticPage {
    QImage image; // Format_ARGB32 scene
    QVector<PageWord> words;
    QRectF chartRegion; // normalized; a curve that must never become text
};

inline void RegisterFixtureFonts()
{
    static bool registered = false;
    if (registered) {
        return;
    }
    registered = true;
    for (const char* path : {"C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/arial.ttf"}) {
        QFontDatabase::addApplicationFont(QString::fromLatin1(path));
    }
}

inline QFont PageFont(int pixelSize)
{
    QFont font(QStringLiteral("Arial"));
    font.setPixelSize(pixelSize);
    font.setStyleStrategy(QFont::PreferAntialias);
    return font;
}

// Dark print on a light page plus one sine curve. `shift` moves all content
// in scene pixels to emulate the page moving under the camera.
inline SyntheticPage MakePage(const QStringList& lines,
                              QPointF shift = {},
                              int width = 1600,
                              int height = 900,
                              int pixelSize = 64,
                              QColor ink = QColor(18, 18, 22),
                              QColor paper = QColor(246, 244, 236))
{
    RegisterFixtureFonts();
    SyntheticPage page;
    page.image = QImage(width, height, QImage::Format_ARGB32);
    page.image.fill(paper);
    QPainter painter(&page.image);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QFont font = PageFont(pixelSize);
    painter.setFont(font);
    painter.setPen(ink);
    const QFontMetricsF metrics(font);
    const qreal space = metrics.horizontalAdvance(QLatin1Char(' '));
    qreal baseline = 110.0 + shift.y();
    for (const QString& line : lines) {
        qreal x = 90.0 + shift.x();
        for (const QString& word : line.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
            const qreal advance = metrics.horizontalAdvance(word);
            painter.drawText(QPointF(x, baseline), word);
            const QRectF box(x, baseline - metrics.ascent(), advance, metrics.ascent() + metrics.descent());
            page.words.push_back({word, QRectF(box.x() / width, box.y() / height,
                                               box.width() / width, box.height() / height)});
            x += advance + space;
        }
        baseline += pixelSize * 1.6;
    }
    // Non-text graphic: a curve in the lower right area.
    const QRectF chart(width * 0.62 + shift.x(), height * 0.62 + shift.y(), width * 0.33, height * 0.3);
    QPainterPath curve;
    for (int i = 0; i <= 200; ++i) {
        const qreal u = i / 200.0;
        const QPointF p(chart.left() + u * chart.width(),
                        chart.center().y() - std::sin(u * 6.283) * chart.height() * 0.4);
        i == 0 ? curve.moveTo(p) : curve.lineTo(p);
    }
    painter.setPen(QPen(QColor(30, 60, 160), 5));
    painter.drawPath(curve);
    painter.end();
    page.chartRegion = QRectF(chart.x() / width, chart.y() / height, chart.width() / width,
                              chart.height() / height);
    return page;
}

// Words drawn at explicit scene-pixel boxes (each box's height sets its
// font size), for reading-order fixtures with mixed line heights.
struct LayoutWord {
    QString text;
    QPointF topLeftPx;
    int pixelSize{};
};

inline SyntheticPage MakeLayoutPage(const QVector<LayoutWord>& layout, int width = 1600, int height = 900)
{
    RegisterFixtureFonts();
    SyntheticPage page;
    page.image = QImage(width, height, QImage::Format_ARGB32);
    page.image.fill(QColor(246, 244, 236));
    QPainter painter(&page.image);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.setPen(QColor(18, 18, 22));
    for (const LayoutWord& word : layout) {
        const QFont font = PageFont(word.pixelSize);
        painter.setFont(font);
        const QFontMetricsF metrics(font);
        const qreal baseline = word.topLeftPx.y() + metrics.ascent();
        painter.drawText(QPointF(word.topLeftPx.x(), baseline), word.text);
        const QRectF box(word.topLeftPx.x(), word.topLeftPx.y(), metrics.horizontalAdvance(word.text),
                         metrics.ascent() + metrics.descent());
        page.words.push_back({word.text, QRectF(box.x() / width, box.y() / height, box.width() / width,
                                                box.height() / height)});
    }
    painter.end();
    return page;
}

// Renders the scene through a ViewTransform into a viewport, as the
// presenter does (letterbox black).
inline QImage RenderView(const QImage& scene, const okuflow::ViewTransform& t, int width, int height)
{
    QImage view(width, height, QImage::Format_ARGB32);
    view.fill(Qt::black);
    QPainter painter(&view);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    const QRectF source(t.sourceX * scene.width(), t.sourceY * scene.height(),
                        t.sourceWidth * scene.width(), t.sourceHeight * scene.height());
    const QRectF target(t.destinationX * width, t.destinationY * height, t.destinationWidth * width,
                        t.destinationHeight * height);
    painter.drawImage(target, scene, source);
    painter.end();
    return view;
}

inline okuflow::uct::ProbeFrame ProbeOf(const QImage& view, const okuflow::ViewTransform& t,
                                        std::uint64_t epoch, std::uint64_t generation,
                                        std::uint64_t frameSeq, std::int64_t timestampMs,
                                        int decimation = 2)
{
    okuflow::uct::SourceStamp stamp;
    stamp.sourceEpoch = epoch;
    stamp.geometryGeneration = generation;
    stamp.frameSeq = frameSeq;
    stamp.timestampMs = timestampMs;
    stamp.viewTransform = t;
    stamp.viewportWidth = static_cast<std::uint32_t>(view.width());
    stamp.viewportHeight = static_cast<std::uint32_t>(view.height());
    return okuflow::uct::MakeProbeFrame(view.constBits(), view.width(), view.height(),
                                        static_cast<int>(view.bytesPerLine()), decimation, stamp);
}

} // namespace uct_test

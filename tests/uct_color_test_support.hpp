// Scripted color evidence, shared by CPU and resident-CUDA regression paths.
// These controlled strokes test sampling, not OCR quality or renderer contrast.
#pragma once

#include <QColor>
#include <QImage>
#include <QRect>
#include <QString>
#include <QVector>
#include <algorithm>
#include <cstdlib>
#include <utility>

namespace uct_test {

struct LocalColorFixture {
    QString name;
    QImage pixels;
    QRect bounds{40, 20, 80, 40};
    QColor ink, paper;
    int tolerance{};
    bool expectSourceColors{true}; // Boundary rows assert raw CPU/GPU parity only.
    bool expectInkCorners{};
};

inline QVector<LocalColorFixture> LocalColorFixtures()
{
    QVector<LocalColorFixture> fixtures;
    const auto add = [&](QString name, QColor ink, QColor paper, QColor exterior,
                         int padding, bool dense, bool antialias, bool gradient = false) {
        LocalColorFixture f{std::move(name), QImage(160, 80, QImage::Format_ARGB32),
                            {40, 20, 80, 40}, ink, paper, gradient ? 4 : 0};
        f.pixels.fill(exterior);
        const QRect surface = padding < 0 ? f.pixels.rect() : f.bounds.adjusted(-padding, -padding, padding, padding);
        for (int y = surface.top(); y <= surface.bottom(); ++y)
            for (int x = surface.left(); x <= surface.right(); ++x) {
                const int delta = gradient ? std::clamp((x - f.bounds.left()) * 8 / (f.bounds.width() - 1) - 4, -4, 4) : 0;
                f.pixels.setPixelColor(x, y, QColor(std::clamp(paper.red() + delta, 0, 255),
                    std::clamp(paper.green() + delta, 0, 255), std::clamp(paper.blue() + delta, 0, 255)));
            }
        for (int y = f.bounds.top(); y <= f.bounds.bottom(); ++y)
            for (int x = f.bounds.left(); x <= f.bounds.right(); ++x) {
                const int localX = x - f.bounds.left(), localY = y - f.bounds.top();
                const int stripe = localX % 16;
                const bool stroke = dense ? localX % 8 < 6
                    : stripe < (antialias ? 8 : 4) && localY >= 4 && localY < f.bounds.height() - 4;
                if (!stroke) continue;
                const int alpha = antialias ? (stripe < 2 ? 256 : stripe < 4 ? 192 : stripe < 6 ? 160 : 128) : 256;
                const QColor background = f.pixels.pixelColor(x, y);
                f.pixels.setPixelColor(x, y, QColor(
                    (ink.red() * alpha + background.red() * (256 - alpha)) / 256,
                    (ink.green() * alpha + background.green() * (256 - alpha)) / 256,
                    (ink.blue() * alpha + background.blue() * (256 - alpha)) / 256));
            }
        fixtures.push_back(std::move(f));
    };
    const QColor dark(18, 25, 36), light(232, 238, 226), green(32, 67, 49), yellow(240, 190, 96);
    add("yellow-button-one-pixel-padding", dark, yellow, green, 1, false, false);
    add("yellow-button-three-pixel-padding", dark, yellow, green, 3, false, false);
    add("yellow-button-exterior-identical-to-ink", dark, yellow, dark, 1, false, false);
    add("light-ink-blue-button-edge", light, QColor(44, 63, 104), green, 1, false, false);
    add("dense75-dark-on-light", dark, light, light, -1, true, false);
    add("dense75-light-on-dark", light, dark, dark, -1, true, false);
    add("dense75-chromatic-ink", QColor(74, 18, 112), light, light, -1, true, false);
    add("aa-dark-endpoint", dark, light, light, -1, false, true);
    add("aa-light-endpoint", light, dark, dark, -1, false, true);
    add("aa-dark-on-small-paper-gradient", dark, light, light, -1, false, true, true);
    add("aa-light-on-small-paper-gradient", light, dark, dark, -1, false, true, true);
    const auto denseGlyph = [&](QString name, char glyph, QColor ink, QColor paper,
                                QColor exterior, int padding = -1) {
        LocalColorFixture f{std::move(name), QImage(160,80,QImage::Format_ARGB32),
                            {60,24,12,16}, ink, paper, 0, true, true};
        f.pixels.fill(padding < 0 ? paper : exterior);
        if (padding >= 0) {
            const QRect surface = f.bounds.adjusted(-padding,-padding,padding,padding);
            for (int y=surface.top(); y<=surface.bottom(); ++y)
                for (int x=surface.left(); x<=surface.right(); ++x) f.pixels.setPixelColor(x,y,paper);
        }
        // Tight 4px stems/bars put ink in every 3x3 corner patch. E covers
        // 83% and H75% of the 2px body lattice; a minority-ink guess inverts.
        for (int y=0; y<f.bounds.height(); ++y)
            for (int x=0; x<f.bounds.width(); ++x) {
                const bool stroke = glyph == 'E' ? x<4 || y<4 || (y>=6 && y<10) || y>=12
                    : x<4 || x>=8 || (y>=6 && y<10);
                if (stroke) f.pixels.setPixelColor(f.bounds.left()+x,f.bounds.top()+y,ink);
            }
        fixtures.push_back(std::move(f));
    };
    for (const char glyph : {'E','H'}) {
        const QString prefix = QStringLiteral("dense-%1-all-corners-ink-").arg(QChar::fromLatin1(glyph));
        denseGlyph(prefix+"dark-on-light", glyph, dark, light, light);
        denseGlyph(prefix+"light-on-dark", glyph, light, dark, dark);
        denseGlyph(prefix+"chromatic-dark", glyph, QColor(74,18,112), yellow, yellow);
        denseGlyph(prefix+"chromatic-light", glyph, QColor(240,220,90), QColor(30,44,80), QColor(30,44,80));
    }
    denseGlyph("dense-E-yellow-button-one-pixel-green-exterior", 'E', dark, yellow, green, 1);
    denseGlyph("dense-H-yellow-button-one-pixel-ink-exterior", 'H', dark, yellow, dark, 1);
    // These rows exercise clipping/count safety only. They deliberately make
    // no claim about which fallback polarity a tiny/empty OCR box should use.
    const auto boundary = [&](QString name, QRect bounds) {
        LocalColorFixture f{std::move(name), QImage(160, 80, QImage::Format_ARGB32),
                            bounds, dark, light, 0, false};
        f.pixels.fill(light);
        for (int y = 0; y < f.pixels.height(); ++y)
            for (int x = 0; x < f.pixels.width(); ++x)
                if (x % 8 < 4 && y % 8 < 4) f.pixels.setPixelColor(x, y, dark);
        fixtures.push_back(std::move(f));
    };
    boundary("boundary-tiny-one-pixel", {40,20,1,1});
    boundary("boundary-tiny-two-pixels", {40,20,2,2});
    boundary("boundary-empty", {40,20,0,0});
    boundary("boundary-outside-negative", {-20,-20,10,10});
    boundary("boundary-outside-positive", {180,90,20,20});
    boundary("boundary-clipped-left", {-9,20,80,40});
    boundary("boundary-clipped-right", {120,20,80,40});
    boundary("boundary-clipped-top", {40,-8,80,40});
    boundary("boundary-clipped-bottom", {40,60,80,40});
    boundary("boundary-clipped-corner", {-8,-8,40,30});
    boundary("boundary-one-pixel-column", {40,20,1,40});
    boundary("boundary-one-pixel-row", {40,20,80,1});
    return fixtures;
}

inline bool SolidInkCorners(const LocalColorFixture& f)
{
    if (f.bounds.width()<3 || f.bounds.height()<3 || !f.pixels.rect().contains(f.bounds)) return false;
    for (int corner=0; corner<4; ++corner) {
        const int left = corner%2 ? f.bounds.right()-2 : f.bounds.left();
        const int top = corner/2 ? f.bounds.bottom()-2 : f.bounds.top();
        for (int y=top; y<top+3; ++y)
            for (int x=left; x<left+3; ++x)
                if (f.pixels.pixelColor(x,y)!=f.ink) return false;
    }
    return true;
}

inline int ColorDistance(const QColor& a, const QColor& b)
{
    return std::max({std::abs(a.red() - b.red()), std::abs(a.green() - b.green()), std::abs(a.blue() - b.blue())});
}

} // namespace uct_test

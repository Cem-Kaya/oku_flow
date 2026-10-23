#pragma once

#include "okuflow/ui/ultra_clear_text_overlay.hpp"
#include "ultra_clear_text_test_support.hpp"

#include <QPainter>
#include <QtTest>

#include <cmath>

// Captured geometry/eligibility, not scripted recognition ground truth. These
// records replay only the model-to-painter boundary of the retained r5 proofs.
// No source footage, model, reference patch or fabricated proof is embedded.
namespace uct_tilted_paint_test {
using okuflow::uct::TextQuad;

struct CapturedWord {
    const char* text;
    std::array<QPointF, 4> quadPx;
    QRectF originalProtectionPx;
    bool glyphEligible;
    bool originallyDrawn;
};

inline QVector<CapturedWord> ButtonRow(bool menu25)
{
    if (!menu25) return {
        {"Left", {{{685.4222786381979,123.82290111515188}, {770.9193069848803,169.6405589427556},
                   {749.7903004780678,209.37173799631597}, {664.2932869733372,163.55407089249226}}},
         {660.6913299560546,115.0767627443586,119.27144840785445,103.68956524985177}, true, true},
        {"Hand", {{{791.1808149627029,180.00984888542277}, {914.6798111435671,245.9491968458944},
                   {893.717424673237,284.63482136480906}, {770.2184284923728,218.69548350048677}}},
         {770.6947631835938,173.0169219970703,143.8702867296007,117.137451171875}, true, true},
        {"Right", {{{1039.5103385910754,312.3581895818459}, {1166.010799279222,379.0412671335209},
                    {1138.0655788858837,431.27772788934664}, {1011.5651181977371,364.5946713151603}}},
         {1021.9729509024784,306.38177963783,148.56367440059273,124.0491196204876}, false, false},
        {"Hand", {{{1186.989231464458,390.0997809228679}, {1314.3743107588157,457.24917370217526},
                   {1292.675415590831,497.80970377045725}, {1165.2903362964735,430.66032739364925}}},
         {1167.6826087688578,383.1907417034281,148.56367440059262,124.04909015523975}, true, false}
    };
    return {
        {"Left", {{{441.75138123787764,809.4050503302052}, {553.3324267592892,819.0833935893995},
                   {549.0445373836103,868.8992129631075}, {437.4634918621988,859.2208520303957}}},
         {441.1061538696289,802.7932189941406,117.13307596842446,72.69745788574221}, true, true},
        {"Hand", {{{577.8640178620778,822.214971913318}, {727.3029181139682,835.1770392517063},
                   {723.1865461604591,883.0002308570419}, {573.7476459085688,870.0381407955697}}},
         {583.2821039835612,815.1253072102865,137.4439259847005,74.45921732584634}, true, true},
        {"Right", {{{879.1073243079377,850.0713825903669}, {1022.6193109815331,862.8156640657276},
                    {1017.2158426138071,922.7720700777494}, {873.7038327730619,910.0277886023888}}},
         {885.6631669264573,838.1858813946063,126.16368572528552,83.52416522686292}, true, true},
        {"Hand", {{{1030.4655423556012,865.1070985156914}, {1157.3228908454082,876.879025209335},
                   {1153.5616629464268,919.8379502387534}, {1028.42483380097,908.0660084223238}}},
         {1027.7184295654297,850.1359939575195,124.6847152709961,87.23653793334961}, true, true}
    };
}

inline TextQuad Quad(const CapturedWord& word, bool normalized)
{
    TextQuad quad;
    quad.valid = true;
    quad.points = word.quadPx;
    if (normalized)
        for (auto& p : quad.points) p = {p.x() / 1920.0, p.y() / 1080.0};
    return quad;
}

inline void CapturedProtectionBaseline(bool menu25)
{
    uct_test::RegisterFixtureFonts();
    QVERIFY(QGuiApplication::platformName() == QStringLiteral("offscreen"));
    QVERIFY(QFontDatabase::families().contains(QStringLiteral("Segoe UI")));
    const auto captured = ButtonRow(menu25);
    QVector<QRectF> protection;
    for (const auto& word : captured)
        if (!word.glyphEligible)
            protection.push_back({word.originalProtectionPx.x() / 1920.0,
                word.originalProtectionPx.y() / 1080.0, word.originalProtectionPx.width() / 1920.0,
                word.originalProtectionPx.height() / 1080.0});
    // The lower Head was actually read as "lead" at 25 s and was ineligible.
    // Its original detector protection survives, without asserting its spelling.
    if (menu25) protection.push_back({15.669783372145432 / 1920.0, 755.0677912785457 / 1080.0,
                                      147.9603614807129 / 1920.0, 93.54628577599158 / 1080.0});
    QVector<okuflow::UltraClearTextOverlay::SceneWord> words;
    for (int i = 0; i < captured.size(); ++i) {
        if (!captured[i].glyphEligible) continue;
        const auto quad = Quad(captured[i], true);
        words.push_back({QString::fromLatin1(captured[i].text), quad.Bounds(), {Qt::black, Qt::white},
                         1, protection, i, quad});
    }
    QCOMPARE(words.size(), menu25 ? 4 : 3);
    QWidget viewport;
    viewport.resize(1920, 1080);
    okuflow::UltraClearTextOverlay overlay(&viewport, &viewport);
    viewport.show();
    const okuflow::ViewTransform transform{0,0,1,1,0,0,1,1,true};
    overlay.SetViewTransform(transform);
    overlay.SetWords(words);
    QTRY_VERIFY(overlay.PaintedGeometryCurrent());
    for (int i = 0; i < captured.size(); ++i) {
        okuflow::UltraClearTextOverlay::PaintedWordGeometry painted;
        const bool found = overlay.PaintedWord(1, i, QString::fromLatin1(captured[i].text), painted);
        QCOMPARE(found, captured[i].glyphEligible);
        if (found) QCOMPARE(painted.drawn, captured[i].originallyDrawn);
    }
    if (menu25) {
        // Source fit, not a font-size clamp: the retained lower Hand is 43 px.
        QVERIFY(std::abs(okuflow::uct::QuadHeight(Quad(captured.back(), false)) - 43.007353229082824) < 1e-7);
    }
    QImage before(viewport.size(), QImage::Format_ARGB32);
    before.fill(QColor(82, 94, 108));
    auto after = before;
    {
        QPainter painter(&after);
        overlay.CaptureSnapshot(transform, after.size()).Paint(painter, after.size());
    }
    QVERIFY(after != before); // Actual glyphs/plates, not word-count evidence.
    QImage liveLayer(viewport.size(), QImage::Format_ARGB32_Premultiplied);
    liveLayer.fill(Qt::transparent);
    overlay.render(&liveLayer);
    for (const auto& mask : protection) {
        const QRectF pixels(mask.x() * 1920, mask.y() * 1080, mask.width() * 1920, mask.height() * 1080);
        const QRect bounds = pixels.toAlignedRect().intersected(after.rect());
        for (int y = bounds.top(); y <= bounds.bottom(); ++y)
            for (int x = bounds.left(); x <= bounds.right(); ++x)
                if (pixels.contains(QPointF(x + .5, y + .5))) {
                    QCOMPARE(after.pixel(x,y), before.pixel(x,y));
                    QCOMPARE(qAlpha(liveLayer.pixel(x,y)), 0);
                }
    }
    if (!menu25) {
        // Isolate the cause: the same proven Hand and font are drawable when
        // the neighbour's mask is absent. Production never removes that mask.
        for (auto& word : words) word.protectedSceneRects.clear();
        const auto paintCount = overlay.PaintCount();
        overlay.SetWords(words);
        QTRY_VERIFY(overlay.PaintCount() > paintCount);
        okuflow::UltraClearTextOverlay::PaintedWordGeometry painted;
        QVERIFY(overlay.PaintedWord(1, 3, QStringLiteral("Hand"), painted));
        QVERIFY(painted.drawn);
    }
}
} // namespace uct_tilted_paint_test

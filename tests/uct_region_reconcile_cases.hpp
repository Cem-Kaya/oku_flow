#pragma once
// Model-free rules of the private region-reconciliation experiment.
#include "../src/common/uct_region_reconcile.hpp"

#include <QtTest>
#include <algorithm>
#include <random>

namespace uct_region_reconcile_test
{
using namespace okuflow::uct;
using region_reconcile::Detection;

inline Detection Box(int region, double x, double y, double width, double height, double degrees = 0)
{
    Detection detection;
    detection.region = region;
    const double angle = degrees * std::numbers::pi / 180, c = std::cos(angle), s = std::sin(angle);
    const QPointF origin(x, y), along(c, s), down(-s, c);
    detection.quad.points = {origin, origin + along * width, origin + along * width + down * height, origin + down * height};
    detection.quad.valid = true;
    return detection;
}
inline Detection Curved(Detection detection)
{
    detection.curved = true;
    return detection;
}

inline QVector<bool> Kept(const QVector<QRect>& regions, QSize source, std::vector<Detection> detections,
                          region_reconcile::Stats* stats = nullptr)
{
    const auto result = region_reconcile::Reconcile(regions, source, detections);
    if (stats) *stats = result;
    QVector<bool> kept;
    for (const auto& detection : detections) kept.push_back(detection.keep);
    return kept;
}

// Whatever is dropped, a kept detection from another region must span it.
inline bool Covered(const QVector<QRect>& regions, QSize source, std::vector<Detection> detections)
{
    auto reconciled = detections;
    region_reconcile::Reconcile(regions, source, reconciled);
    std::vector<region_reconcile::Shape> shapes;
    for (const auto& detection : reconciled) shapes.push_back(region_reconcile::Measure(detection, regions[detection.region], source));
    for (std::size_t i = 0; i < reconciled.size(); ++i) {
        if (reconciled[i].keep) continue;
        bool covered = false;
        for (std::size_t j = 0; j < reconciled.size() && !covered; ++j)
            covered = reconciled[j].keep && region_reconcile::Replaces(reconciled[j], shapes[j], reconciled[i], shapes[i]);
        if (!covered) return false;
    }
    return true;
}

inline void Rules()
{
    const QSize source(1920, 1080);
    // The two tiles DetectorRegions produces for 1920x1080: 1040 wide, 160 overlap.
    const QVector<QRect> regions = DetectorRegions(source);
    QCOMPARE(regions.size(), qsizetype(3));
    QCOMPARE(regions[0], QRect(QPoint{}, source));
    const QRect left = regions[1], right = regions[2];
    QCOMPARE(left.x(), 0);
    QVERIFY(right.x() < left.x() + left.width()); // overlap band
    const int seam = left.x() + left.width(), band = right.x();
    region_reconcile::Stats stats;

    // One region: nothing is ever dropped.
    QCOMPARE(Kept({QRect(0, 0, 1280, 720)}, {1280, 720}, {Box(0, 100, 100, 300, 30), Box(0, 100, 100, 300, 30)}),
             (QVector<bool>{true, true}));
    // The first region must be the whole frame; an unknown layout is left alone.
    QCOMPARE(Kept({left, right}, source, {Box(0, 100, 100, 300, 30), Box(1, 100, 100, 300, 30)}),
             (QVector<bool>{true, true}));
    // An out-of-range region index or an invalid quad leaves everything alone.
    QCOMPARE(Kept(regions, source, {Box(0, 100, 100, 300, 30), Box(7, 100, 100, 300, 30)}),
             (QVector<bool>{true, true}));
    {
        auto invalid = Box(1, 100, 100, 300, 30);
        invalid.quad.valid = false;
        QCOMPARE(Kept(regions, source, {Box(0, 100, 100, 300, 30), invalid}), (QVector<bool>{true, true}));
    }

    // B: a whole-frame line that an unclipped tile detection reproduces.
    QCOMPARE(Kept(regions, source, {Box(0, 98, 198, 404, 34), Box(1, 100, 200, 400, 30)}, &stats),
             (QVector<bool>{false, true}));
    QCOMPARE(stats.wholeDropped, 1);
    QCOMPARE(stats.Kept(), 1);
    // ...but not when the tile detection is shorter: the rest would have no
    // replacement (x 340 to 500 here, a detector that split the line).
    QCOMPARE(Kept(regions, source, {Box(0, 100, 200, 400, 30), Box(1, 100, 200, 240, 30)}),
             (QVector<bool>{true, true}));
    // ...not on another row or at another angle.
    QCOMPARE(Kept(regions, source, {Box(0, 100, 200, 400, 30), Box(1, 100, 236, 400, 30)}),
             (QVector<bool>{true, true}));
    QCOMPARE(Kept(regions, source, {Box(0, 100, 200, 400, 30), Box(1, 100, 200, 400, 30, 14)}),
             (QVector<bool>{true, true}));
    // ...and not when the whole-frame box merged two lines (twice the height).
    QCOMPARE(Kept(regions, source, {Box(0, 100, 200, 400, 64), Box(1, 100, 200, 400, 30), Box(1, 100, 234, 400, 30)}),
             (QVector<bool>{true, true, true}));
    // Curved detections take no part, as the dropped or as the kept side.
    QCOMPARE(Kept(regions, source, {Curved(Box(0, 98, 198, 404, 34)), Box(1, 100, 200, 400, 30)}),
             (QVector<bool>{true, true}));
    QCOMPARE(Kept(regions, source, {Box(0, 98, 198, 404, 34), Curved(Box(1, 100, 200, 400, 30))}),
             (QVector<bool>{true, true}));

    // A: a fragment cut by the left tile's internal edge, spanned by the
    // whole-frame line that continues past that edge. The whole line stays.
    QCOMPARE(Kept(regions, source, {Box(0, 700, 300, 700, 30), Box(1, 700, 300, seam - 700 - 2, 30)}, &stats),
             (QVector<bool>{true, false}));
    QCOMPARE(stats.clippedDropped, 1);
    // Small type (under 24 px): the whole-frame pass may not stand in for the
    // tile fragment...
    QCOMPARE(Kept(regions, source, {Box(0, 700, 300, 700, 22), Box(1, 700, 300, seam - 700 - 2, 18)}),
             (QVector<bool>{true, true}));
    // ...but a larger detection from another tile still may, at any size.
    QCOMPARE(Kept(regions, source, {Box(1, band + 40, 340, seam - band - 42, 18), Box(2, band + 38, 340, 500, 18)}),
             (QVector<bool>{false, true}));
    // The floor is in frame pixels at every frame size: at 3840 wide 20 px
    // type is too small and 30 px type is large enough.
    {
        const QSize large(3840, 2160);
        const auto tiles = DetectorRegions(large);
        const int edge = tiles[1].x() + tiles[1].width();
        QCOMPARE(Kept(tiles, large, {Box(0, 500, 300, 1500, 24), Box(1, 500, 300, edge - 500 - 2, 20)}),
                 (QVector<bool>{true, true}));
        QCOMPARE(Kept(tiles, large, {Box(0, 500, 300, 1500, 36), Box(1, 500, 300, edge - 500 - 2, 30)}),
                 (QVector<bool>{true, false}));
        // A fragment clipped at the bottom of its tile in a two-row layout,
        // and at a corner where two internal edges meet.
        const QRect first = tiles[1];
        const int bottom = first.y() + first.height();
        QCOMPARE(Kept(tiles, large, {Box(0, 200, bottom - 70, 400, 140, 0), Box(1, 200, bottom - 70, 400, 68)}),
                 (QVector<bool>{true, true})); // half the height: not the same line
        QVERIFY(Covered(tiles, large, {Box(0, edge - 500, bottom - 80, 1200, 80), Box(1, edge - 500, bottom - 80, 498, 78),
                                       Box(2, tiles[2].x() + 2, bottom - 80, 700, 78)}));
    }
    // Large type ends well before the edge when the cut falls between letters:
    // more than the 8 px word margin, less than half its height.
    QCOMPARE(Kept(regions, source, {Box(0, 500, 400, 900, 200), Box(1, 500, 400, seam - 500 - 60, 200)}),
             (QVector<bool>{true, false}));
    // The same fragment far from the edge is a detection in its own right.
    QCOMPARE(Kept(regions, source, {Box(0, 500, 400, 900, 200), Box(1, 500, 400, 250, 200)}),
             (QVector<bool>{true, true}));
    // A fragment that sticks out of its neighbour holds text the neighbour
    // never saw: two halves of one long line, no whole-frame detection.
    QCOMPARE(Kept(regions, source, {Box(1, 300, 500, seam - 300 - 2, 30), Box(2, band + 2, 500, 700, 30)}),
             (QVector<bool>{true, true}));
    // A short fragment inside the band that begins before the other tile's part.
    QCOMPARE(Kept(regions, source, {Box(1, band - 60, 600, seam - band + 58, 30), Box(2, band + 2, 600, 700, 30)}),
             (QVector<bool>{true, true}));
    // Both halves and the whole-frame line: the halves go, the line is read once.
    QCOMPARE(Kept(regions, source, {Box(0, 300, 500, 1300, 32), Box(1, 300, 500, seam - 300 - 2, 30),
                                    Box(2, band + 2, 500, 1600 - band - 2, 30)}, &stats),
             (QVector<bool>{true, false, false}));
    QCOMPARE(stats.clippedDropped, 2);
    // A clipped fragment is never the reason to drop a whole-frame line.
    QCOMPARE(Kept(regions, source, {Box(0, 700, 300, 336, 30), Box(1, 700, 300, seam - 700 - 2, 30)}),
             (QVector<bool>{true, true}));
    // A rotated fragment follows the same rule along its own baseline.
    QCOMPARE(Kept(regions, source, {Box(0, 700, 300, 700, 30, 6), Box(1, 700, 300, 338, 30, 6)}),
             (QVector<bool>{true, false}));

    // Chains (constructed by the reviewers). The whole-frame line stands in for
    // a clipped fragment, and a tile detection that covers only part of the
    // whole-frame line must not then remove it.
    QCOMPARE(Kept(regions, source, {Box(0, 700, 300, 700, 30), Box(1, 700, 300, seam - 700 - 2, 30),
                                    Box(2, 1000, 300, 400, 30)}),
             (QVector<bool>{true, false, true}));
    QCOMPARE(Kept(regions, source, {Box(0, 300, 300, 900, 30), Box(1, 300, 300, 520, 30), Box(1, 860, 300, seam - 860 - 2, 30),
                                    Box(2, band + 2, 300, 1200 - band - 2, 30)}),
             (QVector<bool>{true, true, false, false}));
    QCOMPARE(Kept(regions, source, {Box(0, 550, 300, 1000, 30), Box(1, 550, 300, seam - 550 - 2, 30),
                                    Box(2, 925, 300, 625, 30)}),
             (QVector<bool>{true, false, true}));
    // Small type cannot reach the whole-frame line through a larger tile fragment:
    // the whole-frame line may not stand in for it, so it stays.
    QVERIFY(Covered(regions, source, {Box(0, 880, 340, 700, 22), Box(1, band + 40, 340, seam - band - 42, 18),
                                      Box(2, band + 2, 340, 500, 18)}));

    // Different containment tolerances can make a chain even without a split
    // line: the tile covers the taller whole-frame envelope within 10 px,
    // but misses the fragment's start by more than its 7.5 px allowance.
    // Restore the fragment rather than borrowing the removed whole-frame box.
    QCOMPARE(Kept(regions, source, {Box(0, 891, 295, 700, 40),
                                    Box(1, 891, 300, seam - 893, 30),
                                    Box(2, 900, 300, 691, 30)}, &stats),
             (QVector<bool>{false, true, true}));
    QCOMPARE(stats.restored, 1);
    QCOMPARE(stats.Kept(), 2);
    // An envelope that looks straight still cannot stand in for a fitted or
    // refused ribbon. This includes the otherwise attractive title fragment.
    QCOMPARE(Kept(regions, source, {Box(0, 500, 400, 900, 200),
                                    Curved(Box(1, 500, 400, seam - 560, 200))}),
             (QVector<bool>{true, true}));

    // C: one line inside the overlap band, complete in both tiles. The lower
    // region keeps it, whatever the order; the whole-frame copy goes as well.
    const double inBand = band + 30, width = seam - band - 60;
    QCOMPARE(Kept(regions, source, {Box(0, inBand, 700, width, 22), Box(1, inBand, 700, width, 20),
                                    Box(2, inBand, 700, width, 20)}, &stats),
             (QVector<bool>{false, true, false}));
    QCOMPARE(stats.wholeDropped, 1);
    QCOMPARE(stats.tileDropped, 1);
    QCOMPARE(Kept(regions, source, {Box(2, inBand, 700, width, 20), Box(1, inBand, 700, width, 20)}),
             (QVector<bool>{false, true}));
    // The longer of two tile detections stays, whichever tile it came from.
    QCOMPARE(Kept(regions, source, {Box(1, 920, 760, 80, 20), Box(2, 895, 760, 105, 20)}),
             (QVector<bool>{false, true}));
    QCOMPARE(Kept(regions, source, {Box(2, 895, 760, 105, 20), Box(1, 920, 760, 80, 20)}),
             (QVector<bool>{true, false}));

    // The decision depends on geometry only, not on the order of detections.
    // The page has every kind of pair, including tile against tile.
    std::vector<Detection> page;
    for (int row = 0; row < 12; ++row) {
        const double y = 60 + row * 70;
        page.push_back(Box(0, 150, y, 1500, 34));
        page.push_back(Box(1, 150, y, seam - 150 - 3, 30));
        page.push_back(Box(2, band + 3, y, 1650 - band - 3, 30));
        page.push_back(Box(0, 1700, y, 150, 34));
        page.push_back(Box(2, 1700, y + 1, 150, 30));
        page.push_back(Box(1, band + 30, y + 36, 60 + row * 2, 20));
        page.push_back(Box(2, band + 30 - row, y + 36, 60 + row * 3, 20));
    }
    QVERIFY(Covered(regions, source, page));
    std::vector<int> order(page.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = int(i);
    const auto reference = Kept(regions, source, page, &stats);
    QCOMPARE(stats.tileDropped, 12);
    QCOMPARE(reference.count(true), qsizetype(36)); // each long line, short line and band line once
    std::mt19937 random(7);
    for (int trial = 0; trial < 20; ++trial) {
        std::shuffle(order.begin(), order.end(), random);
        std::vector<Detection> shuffled;
        for (const int index : order) shuffled.push_back(page[std::size_t(index)]);
        const auto kept = Kept(regions, source, shuffled);
        for (std::size_t i = 0; i < order.size(); ++i) QCOMPARE(kept[qsizetype(i)], reference[order[i]]);
    }

    // Random layouts: whatever the rules decide, nothing dropped is left without
    // a kept detection that spans it.
    for (int trial = 0; trial < 300; ++trial) {
        std::uniform_real_distribution<double> x(0, 1700), y(20, 1000), w(40, 900), h(14, 90), tilt(-4, 4);
        std::uniform_int_distribution<int> region(0, 2), chance(0, 9);
        std::vector<Detection> layout;
        for (int row = 0; row < 14; ++row) {
            const double top = y(random), height = h(random), start = x(random), length = w(random), angle = tilt(random);
            for (int copy = 0; copy < 4; ++copy) {
                const int index = region(random);
                const QRect area = regions[index];
                const double from = std::max<double>(start + (copy ? x(random) / 20 - 40 : 0), area.x());
                const double to = std::min<double>(start + length - (copy ? x(random) / 25 : 0), area.x() + area.width());
                if (to - from < 20) continue;
                auto box = Box(index, from, top + copy % 2, to - from, height * (index ? 1 : 1.25), angle);
                if (chance(random) == 0) box = Curved(box);
                layout.push_back(box);
            }
        }
        QVERIFY2(Covered(regions, source, layout), qPrintable(QStringLiteral("random layout %1").arg(trial)));
        auto result = layout;
        const auto resultStats = region_reconcile::Reconcile(regions, source, result);
        QCOMPARE(resultStats.Kept(), int(std::count_if(result.begin(), result.end(),
            [](const Detection& detection) { return detection.keep; })));
        for (std::size_t i = 0; i < layout.size(); ++i) {
            QCOMPARE(result[i].quad.points, layout[i].quad.points);
            QCOMPARE(result[i].region, layout[i].region);
            QCOMPARE(result[i].curved, layout[i].curved);
            if (layout[i].curved) QVERIFY(result[i].keep);
        }
        std::vector<std::size_t> permutation(layout.size());
        for (std::size_t i = 0; i < permutation.size(); ++i) permutation[i] = i;
        std::shuffle(permutation.begin(), permutation.end(), random);
        std::vector<Detection> shuffled;
        for (const auto i : permutation) shuffled.push_back(layout[i]);
        region_reconcile::Reconcile(regions, source, shuffled);
        for (std::size_t i = 0; i < shuffled.size(); ++i)
            QCOMPARE(shuffled[i].keep, result[permutation[i]].keep);
    }
}
} // namespace uct_region_reconcile_test

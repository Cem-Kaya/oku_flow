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

inline QVector<bool> Kept(const QVector<QRect>& regions, QSize source, std::vector<Detection> detections,
                          region_reconcile::Stats* stats = nullptr)
{
    const auto result = region_reconcile::Reconcile(regions, source, detections);
    if (stats) *stats = result;
    QVector<bool> kept;
    for (const auto& detection : detections) kept.push_back(detection.keep);
    return kept;
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
    // An out-of-range region index leaves everything alone.
    QCOMPARE(Kept(regions, source, {Box(0, 100, 100, 300, 30), Box(7, 100, 100, 300, 30)}),
             (QVector<bool>{true, true}));

    // B: a whole-frame line that an unclipped tile detection reproduces.
    QCOMPARE(Kept(regions, source, {Box(0, 98, 198, 404, 34), Box(1, 100, 200, 400, 30)}, &stats),
             (QVector<bool>{false, true}));
    QCOMPARE(stats.wholeDropped, 1);
    QCOMPARE(stats.Kept(), 1);
    // ...but not when the tile detection is much shorter: the tile saw less.
    QCOMPARE(Kept(regions, source, {Box(0, 100, 200, 400, 30), Box(1, 100, 200, 180, 30)}),
             (QVector<bool>{true, true}));
    // ...and not on another row or at another angle.
    QCOMPARE(Kept(regions, source, {Box(0, 100, 200, 400, 30), Box(1, 100, 236, 400, 30)}),
             (QVector<bool>{true, true}));
    QCOMPARE(Kept(regions, source, {Box(0, 100, 200, 400, 30), Box(1, 100, 200, 400, 30, 14)}),
             (QVector<bool>{true, true}));

    // A: a fragment cut by the left tile's internal edge, contained by the
    // whole-frame line that continues past that edge. The whole line stays.
    QCOMPARE(Kept(regions, source, {Box(0, 700, 300, 700, 30), Box(1, 700, 300, seam - 700 - 2, 30)}, &stats),
             (QVector<bool>{true, false}));
    QCOMPARE(stats.clippedDropped, 1);
    // Small type: the whole-frame pass may not stand in for the tile fragment...
    QCOMPARE(Kept(regions, source, {Box(0, 700, 300, 700, 22), Box(1, 700, 300, seam - 700 - 2, 18)}),
             (QVector<bool>{true, true}));
    // ...but a larger detection from another tile still may, at any size.
    QCOMPARE(Kept(regions, source, {Box(1, band + 40, 340, seam - band - 42, 18), Box(2, band + 38, 340, 500, 18)}),
             (QVector<bool>{false, true}));
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

    // C: one line inside the overlap band, complete in both tiles. The earlier
    // tile keeps it; the whole-frame copy goes as well.
    const double inBand = band + 30, width = seam - band - 60;
    QCOMPARE(Kept(regions, source, {Box(0, inBand, 700, width, 22), Box(1, inBand, 700, width, 20),
                                    Box(2, inBand, 700, width, 20)}, &stats),
             (QVector<bool>{false, true, false}));
    QCOMPARE(stats.wholeDropped, 1);
    QCOMPARE(stats.tileDropped, 1);

    // The decision depends on geometry only, not on the order of detections.
    std::vector<Detection> page;
    for (int row = 0; row < 12; ++row) {
        const double y = 60 + row * 70;
        page.push_back(Box(0, 150, y, 1500, 34));
        page.push_back(Box(1, 150, y, seam - 150 - 3, 30));
        page.push_back(Box(2, band + 3, y, 1650 - band - 3, 30));
        page.push_back(Box(0, 1700, y, 150, 34));
        page.push_back(Box(2, 1700, y + 1, 150, 30));
    }
    std::vector<int> order(page.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = int(i);
    const auto reference = Kept(regions, source, page);
    QCOMPARE(reference.count(true), qsizetype(24)); // each long line once, each short one once
    std::mt19937 random(7);
    for (int trial = 0; trial < 20; ++trial) {
        std::shuffle(order.begin(), order.end(), random);
        std::vector<Detection> shuffled;
        for (const int index : order) shuffled.push_back(page[std::size_t(index)]);
        const auto kept = Kept(regions, source, shuffled);
        for (std::size_t i = 0; i < order.size(); ++i) {
            // C keeps the earlier tile; no pair in this page is tile against tile.
            QCOMPARE(kept[qsizetype(i)], reference[order[i]]);
        }
    }
}
} // namespace uct_region_reconcile_test

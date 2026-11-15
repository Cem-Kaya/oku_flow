#pragma once
// Cross-region detection reconciliation for full reads (private experiment).
//
// A full read detects text in the whole frame and again in overlapping tiles.
// Ordinarily every detection of every region is recognized and the words are
// reconciled afterwards by overlap and confidence. That recognizes most lines
// more than once, and a confident fragment cut by a tile edge can replace the
// complete word seen by another region.
//
// This decides, from detector geometry alone and before recognition, which
// detections not to recognize:
//   A. a tile detection that ends at one of its tile's internal edges, when a
//      larger detection of the same line from another region contains its
//      whole extent along the line and continues past that edge (the tile saw
//      only part of what the other region saw); a whole-frame detection
//      counts only for type at least kWholeFrameMinimumHeight tall;
//   B. a whole-frame detection that an unclipped tile detection reproduces
//      (the tile has the higher detector resolution);
//   C. the later of two unclipped detections of the same line from two tiles.
// Everything else is recognized and merged exactly as before. No detection is
// moved, joined or invented, and nothing here reads pixels or text.
#include "okuflow/common/ultra_clear_text.hpp"

#include <QRect>
#include <QVector>
#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>
#include <vector>

namespace okuflow::uct::region_reconcile {

struct Detection {
    TextQuad quad; // frame pixels
    int region{};  // index into the regions passed to Reconcile
    bool keep{true};
};
struct Stats {
    int detections{}, clippedDropped{}, wholeDropped{}, tileDropped{};
    int Kept() const { return detections - clippedDropped - wholeDropped - tileDropped; }
};

// Same line, same extent: used for B and C.
inline constexpr double kEquivalentOverlap = .8, kEquivalentAreaRatio = .55;
// A: how far a fragment may stick out of the detection that contains it, and
// how far apart their centre lines may be, in units of the text height.
inline constexpr double kContainTolerance = .25, kContainOffset = .35;
// A: smallest fragment box height, in frame pixels, for which a whole-frame
// detection may replace tile fragments. Chosen among 24, 32 and 40 on twenty
// labelled tuning pages and then checked on 88 held-out pages; see
// docs/uct_okuread_port.md. Tile detections replace each other at any size.
inline constexpr double kWholeFrameMinimumHeight = 24.0;
inline constexpr double kMaximumTurn = .175; // radians, as in word merging
inline constexpr double kMinimumHeightRatio = .5;

enum Edge : unsigned { Left = 1, Top = 2, Right = 4, Bottom = 8 };

struct Shape {
    QRectF bounds;
    QPointF center;
    double area{}, height{}, angle{}, margin{};
    unsigned clipped{};
};

inline Shape Measure(const Detection& detection, const QRect& region, QSize source)
{
    Shape shape;
    shape.bounds = detection.quad.Bounds();
    shape.center = detection.quad.Center();
    shape.area = QuadArea(detection.quad);
    shape.height = QuadHeight(detection.quad);
    shape.angle = QuadAngle(detection.quad);
    // The recognized-word rule drops words within kDetailCropEdgeMarginPx of an
    // internal crop edge. Large type ends farther from the edge than that when
    // the cut falls between letters, so scale the margin with the text height.
    shape.margin = std::max(kDetailCropEdgeMarginPx, .5 * shape.height);
    if (region.left() > 0 && shape.bounds.left() - region.left() < shape.margin) shape.clipped |= Left;
    if (region.top() > 0 && shape.bounds.top() - region.top() < shape.margin) shape.clipped |= Top;
    if (region.x() + region.width() < source.width() &&
        region.x() + region.width() - shape.bounds.right() < shape.margin) shape.clipped |= Right;
    if (region.y() + region.height() < source.height() &&
        region.y() + region.height() - shape.bounds.bottom() < shape.margin) shape.clipped |= Bottom;
    return shape;
}

inline bool SameLine(const Shape& a, const Shape& b)
{
    double turn = std::fmod(std::abs(a.angle - b.angle), std::numbers::pi);
    turn = std::min(turn, std::numbers::pi - turn);
    const double small = std::min(a.height, b.height), large = std::max(a.height, b.height);
    return turn <= kMaximumTurn && small > 0 && small >= kMinimumHeightRatio * large;
}

// `other` continues past every internal edge that clipped `fragment`.
inline bool ContinuesPast(const Shape& fragment, const QRect& region, const Shape& other)
{
    const double margin = fragment.margin;
    return (!(fragment.clipped & Left) || other.bounds.left() < region.left() - margin) &&
           (!(fragment.clipped & Top) || other.bounds.top() < region.top() - margin) &&
           (!(fragment.clipped & Right) || other.bounds.right() > region.x() + region.width() + margin) &&
           (!(fragment.clipped & Bottom) || other.bounds.bottom() > region.y() + region.height() + margin);
}

// `other` spans everything `fragment` spans along the fragment's own baseline
// and lies on the same row. A neighbour that merely overlaps the fragment's
// end does not contain it: the fragment may hold text the neighbour never saw.
inline bool Contains(const Detection& other, const Shape& otherShape, const Detection& fragment, const Shape& shape)
{
    const QPointF along(std::cos(shape.angle), std::sin(shape.angle)), across(-along.y(), along.x());
    const auto extent = [&](const TextQuad& quad) {
        double low = QPointF::dotProduct(quad.points[0], along), high = low;
        for (const QPointF& point : quad.points) {
            const double t = QPointF::dotProduct(point, along);
            low = std::min(low, t); high = std::max(high, t);
        }
        return std::pair{low, high};
    };
    const auto [low, high] = extent(fragment.quad);
    const auto [otherLow, otherHigh] = extent(other.quad);
    const double tolerance = kContainTolerance * shape.height;
    return otherLow <= low + tolerance && otherHigh >= high - tolerance &&
           std::abs(QPointF::dotProduct(otherShape.center - shape.center, across)) <=
               kContainOffset * std::max(shape.height, otherShape.height);
}

// regions[0] must be the whole frame when there is more than one region, as
// DetectorRegions produces. With a single region nothing is dropped.
inline Stats Reconcile(const QVector<QRect>& regions, QSize source, std::vector<Detection>& detections)
{
    Stats stats;
    stats.detections = int(detections.size());
    if (regions.size() < 2 || regions.first() != QRect(QPoint{}, source)) return stats;
    std::vector<Shape> shapes;
    shapes.reserve(detections.size());
    for (const auto& detection : detections) {
        if (detection.region < 0 || detection.region >= regions.size() || !detection.quad.valid) return stats;
        shapes.push_back(Measure(detection, regions[detection.region], source));
    }
    const auto overlap = [&](std::size_t a, std::size_t b) {
        return shapes[a].bounds.intersects(shapes[b].bounds)
            ? QuadIntersectionArea(detections[a].quad, detections[b].quad) : 0.0;
    };
    const auto equivalent = [&](std::size_t a, std::size_t b) {
        const double small = std::min(shapes[a].area, shapes[b].area), large = std::max(shapes[a].area, shapes[b].area);
        return small > 0 && small >= kEquivalentAreaRatio * large && SameLine(shapes[a], shapes[b]) &&
               overlap(a, b) >= kEquivalentOverlap * small;
    };
    // A: clipped tile fragments. Decided from geometry alone, so the outcome
    // does not depend on the order in which detections are visited. Requiring
    // the other detection to be larger makes mutual removal impossible.
    std::vector<bool> fragment(detections.size());
    for (std::size_t i = 0; i < detections.size(); ++i) {
        if (!shapes[i].clipped || shapes[i].area <= 0) continue;
        for (std::size_t j = 0; j < detections.size() && !fragment[i]; ++j) {
            if (detections[j].region == detections[i].region || shapes[j].area <= shapes[i].area) continue;
            // The whole-frame pass sees the frame at a fraction of the tiles'
            // scale. Its boxes for small type are too tall to read well, so it
            // may stand in for tile fragments only when the type is large enough.
            if (detections[j].region == 0 && shapes[i].height < kWholeFrameMinimumHeight) continue;
            fragment[i] = SameLine(shapes[i], shapes[j]) &&
                ContinuesPast(shapes[i], regions[detections[i].region], shapes[j]) &&
                Contains(detections[j], shapes[j], detections[i], shapes[i]);
        }
    }
    for (std::size_t i = 0; i < detections.size(); ++i)
        if (fragment[i]) { detections[i].keep = false; ++stats.clippedDropped; }
    // B: whole-frame detections reproduced by an unclipped tile detection.
    for (std::size_t i = 0; i < detections.size(); ++i) {
        if (detections[i].region != 0 || !detections[i].keep) continue;
        for (std::size_t j = 0; j < detections.size(); ++j) {
            if (detections[j].region == 0 || fragment[j] || shapes[j].clipped || !equivalent(i, j)) continue;
            detections[i].keep = false;
            ++stats.wholeDropped;
            break;
        }
    }
    // C: the same line seen completely by two tiles; the earlier tile keeps it.
    for (std::size_t i = 0; i < detections.size(); ++i) {
        if (detections[i].region == 0 || !detections[i].keep || shapes[i].clipped) continue;
        for (std::size_t j = 0; j < i; ++j) {
            if (detections[j].region == 0 || detections[j].region == detections[i].region ||
                !detections[j].keep || shapes[j].clipped || !equivalent(i, j)) continue;
            detections[i].keep = false;
            ++stats.tileDropped;
            break;
        }
    }
    return stats;
}

} // namespace okuflow::uct::region_reconcile

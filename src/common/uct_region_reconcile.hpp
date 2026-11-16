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
// detections not to recognize. One relation carries every decision:
// detection S may stand in for detection D when both are straight, come from
// different regions, lie on the same line with comparable height, and S spans
// everything D spans along the line (Replaces). Then:
//   A. a tile detection that ends at one of its tile's internal edges is left
//      out when a larger detection stands in for it and continues past that
//      edge (the tile saw only part of what the other region saw);
//   B. a whole-frame detection is left out when an unclipped tile detection
//      stands in for it (the tile has the higher detector resolution);
//   C. an unclipped tile detection is left out when an unclipped detection
//      from another tile stands in for it; if each stands in for the other,
//      the lower region index keeps it.
// Finally every detection left out must still have a KEPT detection standing
// in for it; otherwise it is restored. So no chain of decisions can leave text
// that only a dropped detection covered.
//
// The whole-frame pass sees the frame at a fraction of the tiles' scale, and
// its boxes for small type are too tall to read well. It may therefore stand
// in for a tile detection only when that detection is at least
// kWholeFrameMinimumHeight frame pixels tall.
//
// Curved (ribbon) detections, valid or refused, take no part: their quad is
// only an envelope, and a refused ribbon carries ink protection that must
// reach decoding. Everything not left out is recognized and merged exactly as
// before. No detection is moved, joined or invented, and nothing here reads
// pixels or text.
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
    bool curved{}; // a ribbon was fitted or refused: excluded from every rule
    bool keep{true};
};
struct Stats {
    int detections{}, clippedDropped{}, wholeDropped{}, tileDropped{}, restored{};
    int Kept() const { return detections - clippedDropped - wholeDropped - tileDropped + restored; }
};

inline constexpr double kMaximumTurn = .175; // radians, as in word merging
// The whole-frame pass draws taller boxes (about 1.3 times) than a tile does
// for the same line; a box that merged two lines is about twice as tall.
inline constexpr double kMinimumHeightRatio = .7;
// How far the replaced detection may stick out of the one standing in for it
// along the line (a quarter of its height, at most 48 px: limits of 16, 32, 48
// and none read the labelled pages alike; 48 is the smallest that repairs as
// many large titles as no limit), and how far apart their centre lines may be.
inline constexpr double kContainTolerance = .25, kContainToleranceLimitPx = 48, kContainOffset = .35;
// Smallest box height, in frame pixels, for which the whole-frame pass may
// stand in for a tile detection. Chosen among 24, 32 and 40 on labelled tuning
// pages. A floor in whole-frame detector pixels (12, which is 24 px at 1920
// wide and 48 px at 3840) was measured as well and kept fewer words; see
// docs/uct_okuread_port.md.
inline constexpr double kWholeFrameMinimumHeight = 24.0;

enum Edge : unsigned { Left = 1, Top = 2, Right = 4, Bottom = 8 };

struct Shape {
    QRectF bounds;
    QPointF center;
    double area{}, height{}, angle{}, margin{}, low{}, high{}; // low/high: extent along its own baseline
    unsigned clipped{};
};

inline std::pair<double, double> Extent(const TextQuad& quad, double angle)
{
    const QPointF along(std::cos(angle), std::sin(angle));
    double low = QPointF::dotProduct(quad.points[0], along), high = low;
    for (const QPointF& point : quad.points) {
        const double t = QPointF::dotProduct(point, along);
        low = std::min(low, t);
        high = std::max(high, t);
    }
    return {low, high};
}

inline Shape Measure(const Detection& detection, const QRect& region, QSize source)
{
    Shape shape;
    shape.bounds = detection.quad.Bounds();
    shape.center = detection.quad.Center();
    shape.area = QuadArea(detection.quad);
    shape.height = QuadHeight(detection.quad);
    shape.angle = QuadAngle(detection.quad);
    const auto extent = Extent(detection.quad, shape.angle);
    shape.low = extent.first;
    shape.high = extent.second;
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

// `other` continues past every internal edge that clipped `fragment`.
inline bool ContinuesPast(const Shape& fragment, const QRect& region, const Shape& other)
{
    const double margin = fragment.margin;
    return (!(fragment.clipped & Left) || other.bounds.left() < region.left() - margin) &&
           (!(fragment.clipped & Top) || other.bounds.top() < region.top() - margin) &&
           (!(fragment.clipped & Right) || other.bounds.right() > region.x() + region.width() + margin) &&
           (!(fragment.clipped & Bottom) || other.bounds.bottom() > region.y() + region.height() + margin);
}

// May `stand` be recognized in place of `gone`? Geometry only; which of the
// two is kept is decided by the rules below.
inline bool Replaces(const Detection& stand, const Shape& standShape, const Detection& gone, const Shape& goneShape)
{
    if (stand.curved || gone.curved || stand.region == gone.region) return false;
    if (standShape.area <= 0 || goneShape.area <= 0) return false;
    double turn = std::fmod(std::abs(standShape.angle - goneShape.angle), std::numbers::pi);
    turn = std::min(turn, std::numbers::pi - turn);
    const double small = std::min(standShape.height, goneShape.height), large = std::max(standShape.height, goneShape.height);
    if (turn > kMaximumTurn || small <= 0 || small < kMinimumHeightRatio * large) return false;
    // Small type: the whole-frame pass is not a substitute for a tile.
    if (stand.region == 0 && goneShape.height < kWholeFrameMinimumHeight) return false;
    // Same row: centre lines close, measured across the replaced detection's baseline.
    const QPointF across(-std::sin(goneShape.angle), std::cos(goneShape.angle));
    if (std::abs(QPointF::dotProduct(standShape.center - goneShape.center, across)) > kContainOffset * small) return false;
    // Everything the replaced detection spans along its baseline is spanned.
    const auto extent = Extent(stand.quad, goneShape.angle);
    const double tolerance = std::min(kContainTolerance * goneShape.height, kContainToleranceLimitPx);
    return extent.first <= goneShape.low + tolerance && extent.second >= goneShape.high - tolerance;
}

// regions[0] must be the whole frame when there is more than one region, as
// DetectorRegions produces. With one region, or an unexpected layout, nothing
// is dropped. The outcome does not depend on the order of `detections`.
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
    const std::size_t count = detections.size();
    const auto replaces = [&](std::size_t stand, std::size_t gone) {
        return Replaces(detections[stand], shapes[stand], detections[gone], shapes[gone]);
    };
    enum class Reason : unsigned char { Kept, Clipped, Whole, Tile };
    std::vector<Reason> reason(count, Reason::Kept);
    // Each rule is a statement about pairs, evaluated on the full input, so no
    // rule sees another rule's outcome and visiting order cannot matter.
    for (std::size_t i = 0; i < count; ++i) {
        if (detections[i].curved) continue;
        const bool whole = detections[i].region == 0, clipped = shapes[i].clipped != 0;
        for (std::size_t j = 0; j < count && reason[i] == Reason::Kept; ++j) {
            if (j == i || !replaces(j, i)) continue;
            const bool otherWhole = detections[j].region == 0, otherClipped = shapes[j].clipped != 0;
            if (whole) { // B
                if (!otherWhole && !otherClipped) reason[i] = Reason::Whole;
            } else if (clipped) { // A
                if (shapes[j].area > shapes[i].area && ContinuesPast(shapes[i], regions[detections[i].region], shapes[j]))
                    reason[i] = Reason::Clipped;
            } else if (!otherWhole && !otherClipped) { // C
                if (!replaces(i, j) || detections[j].region < detections[i].region) reason[i] = Reason::Tile;
            }
        }
    }
    // Coverage: a detection stays out only while a kept detection stands in
    // for it. Restorations of one pass are decided against the kept set at the
    // start of that pass, and the kept set only grows, so this terminates and
    // is independent of order.
    std::vector<bool> keep(count);
    for (std::size_t i = 0; i < count; ++i) keep[i] = reason[i] == Reason::Kept;
    for (bool changed = true; changed;) {
        changed = false;
        std::vector<std::size_t> restore;
        for (std::size_t i = 0; i < count; ++i) {
            if (keep[i]) continue;
            bool covered = false;
            for (std::size_t j = 0; j < count && !covered; ++j) covered = keep[j] && j != i && replaces(j, i);
            if (!covered) restore.push_back(i);
        }
        for (const std::size_t i : restore) {
            keep[i] = true;
            ++stats.restored;
            changed = true;
        }
    }
    for (std::size_t i = 0; i < count; ++i) {
        detections[i].keep = keep[i];
        stats.clippedDropped += reason[i] == Reason::Clipped;
        stats.wholeDropped += reason[i] == Reason::Whole;
        stats.tileDropped += reason[i] == Reason::Tile;
    }
    return stats;
}

} // namespace okuflow::uct::region_reconcile

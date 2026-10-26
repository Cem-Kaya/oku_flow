#pragma once
#include "okuflow/common/uct_ink_outline_types.hpp"
#include <QColor>
#include <QImage>
#include <QPainterPath>
#include <QPolygonF>
#include <QRectF>
#include <QVector>
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <unordered_map>
#include <vector>

namespace okuflow::uct::ink
{
struct Contour
{
    QPolygonF points;
    double area{}, perimeter{};
    QRectF bounds;
};
using Segments = std::array<std::vector<Segment>, Levels>;
inline bool Valid(const Region &r)
{
    for (int c = 0; c < 3; ++c)
        if (r.foreground[c] < 0 || r.foreground[c] > 255 || r.background[c] < 0 || r.background[c] > 255)
            return false;
    return r.x >= 0 && r.y >= 0 && r.w >= 12 && r.h >= 12 && r.w <= MaximumWidth && r.h <= MaximumHeight &&
           r.w * r.h <= MaximumPixels;
}
inline std::vector<Contour> Trace(const std::vector<Segment> &segments, bool &ok, bool &limited)
{
    ok = false;
    std::vector<Contour> loops;
    limited = segments.size() > MaximumSegments;
    if (segments.empty() || limited)
        return loops;
    const auto key = [](int x, int y) { return (std::uint64_t(std::uint32_t(x)) << 32) | std::uint32_t(y); };
    std::unordered_map<std::uint64_t, int> outgoing;
    outgoing.reserve(segments.size());
    for (int i = 0; i < int(segments.size()); ++i)
        if (!outgoing.emplace(key(segments[i].ax, segments[i].ay), i).second)
            return {};
    std::vector<bool> used(segments.size());
    for (int i = 0; i < int(segments.size()); ++i)
    {
        if (used[i])
            continue;
        Contour loop;
        int current = i;
        long long area = 0;
        for (std::size_t visit = 0;; ++visit)
        {
            if (visit >= segments.size() || used[current])
                return {};
            used[current] = true;
            const auto &s = segments[current];
            loop.points.push_back({s.ax / 256., s.ay / 256.});
            area += static_cast<long long>(s.ax) * s.by - static_cast<long long>(s.bx) * s.ay;
            loop.perimeter += std::hypot(s.bx - s.ax, s.by - s.ay) / 256.;
            const auto next = outgoing.find(key(s.bx, s.by));
            if (next == outgoing.end())
                return {};
            if (next->second == i)
                break;
            current = next->second;
        }
        if (loop.points.size() < 3 || !area)
            return {};
        loop.area = area / (2. * 256 * 256);
        loop.bounds = loop.points.boundingRect();
        loops.push_back(std::move(loop));
        if (loops.size() > 256)
        {
            limited = true;
            return {};
        }
    }
    ok = true;
    return loops;
}
inline bool Contains(const Contour &outer, const Contour &inner)
{
    return !inner.points.empty() && outer.bounds.contains(inner.points[0]) &&
           outer.points.containsPoint(inner.points[0], Qt::OddEvenFill);
}
inline int Owner(const Contour &inner, const std::vector<Contour> &outers, bool positive)
{
    int owner = -1;
    for (int i = 0; i < int(outers.size()); ++i)
    {
        const auto &outer = outers[i];
        if ((outer.area > 0) == positive &&
            (owner < 0 || std::abs(outer.area) < std::abs(outers[owner].area)) && Contains(outer, inner))
            owner = i;
    }
    return owner;
}
// Resolved structure must correspond across thresholds. Tiny islands need a
// strict-level survivor before amplification; omitted islands remain original.
// Holes in every painted component stay holes, including sub-four-pixel ones.
inline Outline Assemble(const Region &region, const Summary &summary, const Segments &segments)
{
    Outline out;
    out.bounds = {double(region.x), double(region.y), double(region.w), double(region.h)};
    out.foreground = {region.foreground[0], region.foreground[1], region.foreground[2]};
    out.background = {region.background[0], region.background[1], region.background[2]};
    if (!Valid(region))
        return out;
    if (summary.invalid)
    {
        out.refusal = summary.invalid == 1 ? Refusal::LowContrast : Refusal::OpenContour;
        return out;
    }
    if (summary.border)
    {
        out.refusal = Refusal::BorderInk;
        return out;
    }
    for (const auto &level : segments)
        if (level.size() > MaximumSegments)
        {
            out.refusal = Refusal::ContourLimit;
            return out;
        }
    std::array<std::vector<Contour>, Levels> contours;
    for (int k = 0; k < Levels; ++k)
    {
        if (segments[k].size() > MaximumSegments)
        {
            out.refusal = Refusal::ContourLimit;
            return out;
        }
        for (const auto &s : segments[k])
            if (s.ax < 128 || s.ay < 128 || s.bx < 128 || s.by < 128 || s.ax > region.w * 256 - 128 ||
                s.bx > region.w * 256 - 128 || s.ay > region.h * 256 - 128 || s.by > region.h * 256 - 128)
            {
                out.refusal = Refusal::OpenContour;
                return out;
            }
        bool valid = false, limited = false;
        contours[k] = Trace(segments[k], valid, limited);
        if (!valid)
        {
            out.refusal = limited ? Refusal::ContourLimit : Refusal::OpenContour;
            return out;
        }
        for (const auto &loop : contours[k])
            if (std::abs(loop.area) >= 4)
                (loop.area > 0 ? out.components[k] : out.holes[k])++;
    }
    if (out.components[2] < 1 || out.components[2] > 64)
    {
        out.refusal = Refusal::Components;
        return out;
    }
    for (int i = 0; i < int(contours[0].size()); ++i)
        if (const auto &low = contours[0][i]; low.area >= 4)
        {
            QRectF centralBounds;
            for (const auto &center : contours[2])
                if (center.area >= 4 && Owner(center, contours[0], true) == i)
                    centralBounds =
                        centralBounds.isNull() ? center.bounds : centralBounds.united(center.bounds);
            const double dx = std::max(2., .15 * low.bounds.width()),
                         dy = std::max(2., .15 * low.bounds.height());
            if (centralBounds.isEmpty() || centralBounds.left() - low.bounds.left() > dx ||
                low.bounds.right() - centralBounds.right() > dx ||
                centralBounds.top() - low.bounds.top() > dy ||
                low.bounds.bottom() - centralBounds.bottom() > dy)
            {
                out.refusal = Refusal::FaintInk;
                return out;
            }
        }
    for (int k : {1, 3})
    {
        if (out.components[k] != out.components[2])
        {
            out.refusal = Refusal::Components;
            return out;
        }
        if (out.holes[k] != out.holes[2])
        {
            out.refusal = Refusal::Holes;
            return out;
        }
        const int loose = k == 1 ? 1 : 2, strict = k == 1 ? 2 : 3;
        std::vector<int> components(contours[loose].size()), holes(contours[strict].size());
        for (const auto &b : contours[strict])
            if (b.area >= 4)
            {
                const int owner = Owner(b, contours[loose], true);
                if (owner < 0)
                {
                    out.refusal = Refusal::Components;
                    return out;
                }
                ++components[owner];
            }
        for (const auto &b : contours[loose])
            if (b.area <= -4)
            {
                const int owner = Owner(b, contours[strict], false);
                if (owner < 0)
                {
                    out.refusal = Refusal::Holes;
                    return out;
                }
                ++holes[owner];
            }
        for (int i = 0; i < int(components.size()); ++i)
            if (contours[loose][i].area >= 4 && components[i] != 1)
            {
                out.refusal = Refusal::Components;
                return out;
            }
        for (int i = 0; i < int(holes.size()); ++i)
            if (contours[strict][i].area <= -4 && holes[i] != 1)
            {
                out.refusal = Refusal::Holes;
                return out;
            }
    }
    std::vector<double> areas, perimeters, strokes;
    for (const auto &contour : contours[2])
    {
        areas.push_back(contour.area);
        perimeters.push_back(contour.perimeter);
    }
    // Assign each hole to its nearest containing ink component once. Repeating
    // an inner-component search for every outer/hole pair made this cubic.
    for (const auto &hole : contours[2])
        if (hole.area < 0)
        {
            int owner = -1;
            for (int i = 0; i < int(contours[2].size()); ++i)
            {
                const auto &outer = contours[2][i];
                if (outer.area > 0 && (owner < 0 || outer.area < contours[2][owner].area) &&
                    Contains(outer, hole))
                    owner = i;
            }
            if (owner >= 0)
            {
                areas[owner] += hole.area;
                perimeters[owner] += hole.perimeter;
            }
        }
    for (int i = 0; i < int(contours[2].size()); ++i)
        if (contours[2][i].area >= 4)
            strokes.push_back(2 * areas[i] / perimeters[i]);
    std::sort(strokes.begin(), strokes.end());
    if (strokes.empty() || strokes[strokes.size() / 2] < 1.5)
    {
        out.refusal = Refusal::ThinInk;
        return out;
    }
    auto geometry = std::make_shared<Geometry>();
    geometry->path.setFillRule(Qt::WindingFill);
    std::vector<bool> paint(contours[2].size(), true);
    for (int i = 0; i < int(contours[2].size()); ++i)
        if (contours[2][i].area > 0 && contours[2][i].area < 4)
        {
            paint[i] = std::any_of(contours[3].begin(), contours[3].end(), [&](const auto &strict) {
                return strict.area > 0 && Owner(strict, contours[2], true) == i;
            });
        }
    for (int i = 0; i < int(contours[2].size()); ++i)
        if (contours[2][i].area < 0)
        {
            const int owner = Owner(contours[2][i], contours[2], true);
            paint[i] = owner >= 0 && paint[owner]; // never emit a negative contour without its ink owner
        }
    for (int i = 0; i < int(contours[2].size()); ++i)
    {
        if (!paint[i])
            continue;
        auto &contour = contours[2][i];
        out.vertices += contour.points.size();
        if (out.vertices > MaximumVertices)
        {
            out.refusal = Refusal::ContourLimit;
            return out;
        }
        contour.points.translate(region.x, region.y);
        geometry->loops.push_back(contour.points);
        geometry->path.addPolygon(contour.points);
        geometry->path.closeSubpath();
    }
    out.geometry = std::move(geometry);
    out.refusal = Refusal::None;
    return out;
}
inline Outline Build(const QImage &image, const Region &region)
{
    if (!Valid(region) || region.x > image.width() - region.w || region.y > image.height() - region.h)
        return {};
    const QImage pixels = image.convertToFormat(QImage::Format_ARGB32);
    std::vector<unsigned char> grid(std::size_t(region.w) * region.h);
    int histogram[256]{};
    for (int y = 0; y < region.h; ++y)
        for (int x = 0; x < region.w; ++x)
        {
            const auto value = Coverage(pixels.constScanLine(y + region.y) + (x + region.x) * 4, region);
            grid[std::size_t(y) * region.w + x] = value;
            ++histogram[value];
        }
    auto summary = Thresholds(histogram, region.w * region.h, region);
    Segments segments;
    if (!summary.invalid)
    {
        for (int y = 0; y < region.h; ++y)
            for (int x = 0; x < region.w; ++x)
                if ((x == 0 || y == 0 || x + 1 == region.w || y + 1 == region.h) &&
                    grid[std::size_t(y) * region.w + x] > summary.cuts[0])
                    summary.border = 1;
        for (int y = 0; y + 1 < region.h; ++y)
            for (int x = 0; x + 1 < region.w; ++x)
            {
                const int at = y * region.w + x;
                const int q[]{grid[at], grid[at + 1], grid[at + region.w + 1], grid[at + region.w]};
                for (int k = 0; k < Levels; ++k)
                {
                    Segment emitted[2];
                    const int n = Cell(q, summary.cuts[k], x, y, emitted);
                    if (n < 0)
                    {
                        Outline refused;
                        refused.refusal = Refusal::OpenContour;
                        return refused;
                    }
                    for (int i = 0; i < n; ++i)
                        segments[k].push_back(emitted[i]);
                    if (segments[k].size() > MaximumSegments)
                    {
                        Outline refused;
                        refused.refusal = Refusal::ContourLimit;
                        return refused;
                    }
                }
            }
    }
    return Assemble(region, summary, segments);
}
} // namespace okuflow::uct::ink

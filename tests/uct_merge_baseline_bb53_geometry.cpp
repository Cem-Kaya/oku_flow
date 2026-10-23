// Exact bb53ac884c24d15f0c77b2dc43cace0e8e650c24 bodies; Frozen names only.
#include "uct_merge_baseline_bb53.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
namespace okuflow::uct::merge_replay {
namespace {
double PolygonArea(const QVector<QPointF>& polygon)
{
    double twice = 0.0;
    for (qsizetype i = 0; i < polygon.size(); ++i) {
        const QPointF& a = polygon[i];
        const QPointF& b = polygon[(i + 1) % polygon.size()];
        twice += a.x() * b.y() - b.x() * a.y();
    }
    return std::abs(twice) * 0.5;
}
bool FinitePoints(const TextQuad& quad)
{
    return std::all_of(quad.points.begin(), quad.points.end(), [](const QPointF& point) {
        return std::isfinite(point.x()) && std::isfinite(point.y());
    });
}
} // namespace

double FrozenQuadArea(const TextQuad& quad)
{
    if (!quad.valid || !FinitePoints(quad)) return 0.0;
    double turn = 0.0;
    for (int i = 0; i < 4; ++i) {
        const auto a = quad.points[(i + 1) % 4] - quad.points[i];
        const auto b = quad.points[(i + 2) % 4] - quad.points[(i + 1) % 4];
        const double cross = a.x() * b.y() - a.y() * b.x();
        if (cross == 0.0 || (turn != 0.0 && std::signbit(cross) != std::signbit(turn))) return 0.0;
        turn = cross;
    }
    return PolygonArea({quad.points[0], quad.points[1], quad.points[2], quad.points[3]});
}

double FrozenQuadIntersectionArea(const TextQuad& a, const TextQuad& b)
{
    if (FrozenQuadArea(a) <= 0.0 || FrozenQuadArea(b) <= 0.0) return 0.0;
    if (!a.Bounds().intersects(b.Bounds())) return 0.0;
    // Convex clipping; both inputs are convex rectangles/parallelograms. The
    // clip polygon's winding decides which side is inside.
    QVector<QPointF> subject{a.points[0], a.points[1], a.points[2], a.points[3]};
    const QVector<QPointF> clip{b.points[0], b.points[1], b.points[2], b.points[3]};
    double winding = 0.0;
    for (int i = 0; i < 4; ++i) {
        const QPointF& p = clip[i];
        const QPointF& q = clip[(i + 1) % 4];
        winding += p.x() * q.y() - q.x() * p.y();
    }
    if (winding == 0.0) return 0.0;
    const double sign = winding > 0.0 ? 1.0 : -1.0;
    for (int edge = 0; edge < 4 && !subject.isEmpty(); ++edge) {
        const QPointF p = clip[edge], q = clip[(edge + 1) % 4];
        const auto side = [&](const QPointF& r) {
            return sign * ((q.x() - p.x()) * (r.y() - p.y()) - (q.y() - p.y()) * (r.x() - p.x()));
        };
        QVector<QPointF> next;
        for (qsizetype i = 0; i < subject.size(); ++i) {
            const QPointF& current = subject[i];
            const QPointF& previous = subject[(i + subject.size() - 1) % subject.size()];
            const double sc = side(current), sp = side(previous);
            if (sc >= 0.0) {
                if (sp < 0.0) next.push_back(previous + (current - previous) * (sp / (sp - sc)));
                next.push_back(current);
            } else if (sp >= 0.0) {
                next.push_back(previous + (current - previous) * (sp / (sp - sc)));
            }
        }
        subject = std::move(next);
    }
    return subject.size() < 3 ? 0.0 : PolygonArea(subject);
}
} // namespace okuflow::uct::merge_replay

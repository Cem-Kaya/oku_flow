#pragma once

#include "ultra_clear_text_test_support.hpp"
#include <QLineF>

namespace uct_quad_reference_test {
using namespace okuflow::uct;
struct Fixture {
    QImage image;
    TextQuad pixels;
    TextQuad scene;
};
inline Fixture Render(double angle, bool changed = false, bool perspective = false)
{
    uct_test::RegisterFixtureFonts();
    Fixture f;
    f.image = QImage(640, 360, QImage::Format_ARGB32);
    f.image.fill(QColor(246, 244, 236));
    const QPointF origin(170, 110), u(std::cos(angle), std::sin(angle)), v(-u.y(), u.x());
    f.pixels = {{origin, origin + u * 240, origin + u * 240 + v * 48, origin + v * 48}, true};
    if (perspective) f.pixels.points[2] -= v * 4;
    QPainterPath glyphs;
    QFont font(QStringLiteral("Segoe UI")); font.setPixelSize(44);
    glyphs.addText({}, font, changed ? QStringLiteral("Nothing") : QStringLiteral("Reading"));
    const QRectF bounds = glyphs.boundingRect();
    QPolygonF from, to;
    from << bounds.topLeft() << bounds.topRight() << bounds.bottomRight() << bounds.bottomLeft();
    for (const auto& point : f.pixels.points) to << point;
    QTransform transform;
    if (!QTransform::quadToQuad(from, to, transform)) return {};
    QPainter painter(&f.image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setTransform(transform);
    painter.fillPath(glyphs, QColor(18, 18, 22));
    f.scene = ViewQuadToScene({0,0,1,1,0,0,1,1,true},640,360,f.pixels);
    return f;
}
inline SourceStamp Stamp(int frame)
{
    SourceStamp stamp;
    stamp.sourceEpoch = stamp.geometryGeneration = 1;
    stamp.frameSeq = frame; stamp.timestampMs = frame * 33;
    stamp.viewportWidth = 640; stamp.viewportHeight = 360;
    stamp.viewTransform = {0,0,1,1,0,0,1,1,true};
    return stamp;
}
inline ProbeFrame Probe(const Fixture& f, int frame, int decimation)
{
    return MakeProbeFrame(f.image.constBits(),640,360,f.image.bytesPerLine(),decimation,Stamp(frame));
}
inline ReferencePatch Reproject(ReferencePatch patch, const ProbeFrame& target, const TextQuad& quad)
{
    const auto geometry = PlanQuadReference(target.stamp,target.Width(),target.Height(),quad);
    patch.sceneRect = geometry.sceneRect; patch.sceneQuad = geometry.sceneQuad;
    patch.valid &= geometry.sceneQuad.valid;
    return patch;
}
} // namespace uct_quad_reference_test

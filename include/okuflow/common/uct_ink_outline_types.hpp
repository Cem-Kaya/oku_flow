#pragma once
#include "okuflow/common/uct_ink_outline_core.hpp"
#include <QColor>
#include <QPainterPath>
#include <QPolygonF>
#include <QRectF>
#include <QVector>
#include <array>
#include <memory>

namespace okuflow::uct::ink
{
enum class Refusal
{
    None,
    Bounds,
    LowContrast,
    BorderInk,
    ContourLimit,
    OpenContour,
    Components,
    Holes,
    FaintInk,
    ThinInk
};
inline const char *Name(Refusal r)
{
    switch (r)
    {
    case Refusal::None:
        return "drawn";
    case Refusal::Bounds:
        return "bounds";
    case Refusal::LowContrast:
        return "low_contrast";
    case Refusal::BorderInk:
        return "border_ink";
    case Refusal::ContourLimit:
        return "contour_limit";
    case Refusal::OpenContour:
        return "open_contour";
    case Refusal::Components:
        return "unstable_components";
    case Refusal::Holes:
        return "unstable_holes";
    case Refusal::FaintInk:
        return "faint_ink";
    case Refusal::ThinInk:
        return "thin_ink";
    }
    return "unknown";
}
struct Geometry
{
    QPainterPath path;
    QVector<QPolygonF> loops;
};
// Analysis pixels before preparation, scene coordinates after preparation.
// This is source-derived drawing geometry, never recognized/accessible text.
struct Outline
{
    QRectF bounds;
    QColor foreground, background;
    std::shared_ptr<const Geometry> geometry;
    Refusal refusal{Refusal::Bounds};
    std::array<int, Levels> components{}, holes{};
    int vertices{};
    std::uint64_t transferBytes{};
};
struct SceneOutline
{
    Outline outline;
    QPointF offset;
    std::uint64_t lineId{};
};
} // namespace okuflow::uct::ink

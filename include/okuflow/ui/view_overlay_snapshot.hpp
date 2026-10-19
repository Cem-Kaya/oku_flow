#pragma once

#include "okuflow/common/annotation_model.hpp"
#include "okuflow/ui/uct_overlay_snapshot.hpp"

#include <QPainter>

namespace okuflow {

// The overlays belonging to one processed photo/readback request. Value
// ownership preserves the clicked view while an asynchronous writer runs.
// UCT plates go below ink; UI chrome and selection handles are not exported.
struct ViewOverlaySnapshot {
    UctOverlaySnapshot text;
    QVector<AnnotationStroke> strokes;
    ViewTransform transform;

    void Paint(QPainter& painter, const QSize& imageSize) const
    {
        text.Paint(painter, imageSize);
        if (!strokes.isEmpty() && transform.valid)
            RenderAnnotationStrokes(painter, strokes, transform, imageSize);
    }
};

} // namespace okuflow

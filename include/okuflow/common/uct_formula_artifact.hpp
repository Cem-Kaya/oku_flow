#pragma once
#include <QColor>
#include <QPainterPath>
#include <QRectF>
#include <QString>
#include <memory>
#include <cmath>

namespace okuflow::uct::formula {
// A recognized whole formula, separate from OCR words and non-text ink sketches.
// Path coordinates are normalized to their own ink bounds, then mapped through
// the same scene/view transform as UCT. No inference or typesetting during paint.
struct Artifact {
    QString latex;
    std::shared_ptr<const QPainterPath> path;
    QRectF sceneRect;
    QRectF glyphSceneRect;
    QColor foreground, background;
    bool IsValid() const {
        const auto valid=[](const QRectF& b){return !b.isEmpty() && std::isfinite(b.x()) &&
            std::isfinite(b.y()) && std::isfinite(b.width()) && std::isfinite(b.height());};
        return path && !path->isEmpty() && path->elementCount()<=200000 && valid(sceneRect) &&
            valid(glyphSceneRect) && sceneRect.adjusted(-1e-6,-1e-6,1e-6,1e-6).contains(glyphSceneRect) &&
            foreground.isValid() && background.isValid() && !latex.isEmpty();
    }
};
struct Layout { std::shared_ptr<const QPainterPath> path;QString error;QSizeF size; };
Layout Typeset(const QString& latex);
} // namespace okuflow::uct::formula

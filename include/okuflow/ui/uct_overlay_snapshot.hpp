#pragma once

#include <QPicture>
#include <QSize>

QT_BEGIN_NAMESPACE
class QPainter;
QT_END_NAMESPACE

namespace okuflow {

// Immutable vector drawing commands frozen on the UI thread for one capture.
// Contains no widgets or borrowed scene data. It can be painted by the image
// writer after live words, style and viewport have changed.
struct UctOverlaySnapshot {
    QPicture picture{};
    QSize viewportSize;

    bool IsEmpty() const noexcept { return picture.isNull() || viewportSize.isEmpty(); }
    void Paint(QPainter& painter, const QSize& imageSize) const;
};

} // namespace okuflow

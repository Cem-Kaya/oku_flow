#pragma once

#include "okuflow/common/uct_windows_ocr.hpp"
#include "okuflow/common/ultra_clear_text.hpp"

#include <cmath>

namespace okuflow::uct::windows_ocr {

// Windows OCR boxes describe the deskewed image. Undo that coordinate change
// in the actual sampled image first, then undo its possibly rounded resize.
// See Windows.Media.Ocr.OcrResult.TextAngle and OcrWord.BoundingRect.
inline TextQuad SourceWordQuad(const Word& word, QSize sampled, QSize original,
                               std::optional<double> textAngleDegrees)
{
    if (sampled.isEmpty() || original.isEmpty() ||
        !std::isfinite(word.x) || !std::isfinite(word.y) ||
        !std::isfinite(word.width) || !std::isfinite(word.height) ||
        word.width <= 0 || word.height <= 0 ||
        (textAngleDegrees && !std::isfinite(*textAngleDegrees))) return {};

    TextQuad quad = TextQuad::FromRect(QRectF(word.x, word.y, word.width, word.height));
    const double angle = std::remainder(textAngleDegrees.value_or(0.0), 360.0) *
                         3.14159265358979323846 / 180.0;
    const double cosine = std::cos(angle), sine = std::sin(angle);
    const QPointF centre(sampled.width() / 2.0, sampled.height() / 2.0);
    const double scaleX = double(original.width()) / sampled.width();
    const double scaleY = double(original.height()) / sampled.height();
    for (auto& point : quad.points) {
        const QPointF delta = point - centre;
        // Positive rotation is clockwise in image coordinates (Y down).
        point = QPointF((centre.x() + cosine * delta.x() - sine * delta.y()) * scaleX,
                        (centre.y() + sine * delta.x() + cosine * delta.y()) * scaleY);
        if (!std::isfinite(point.x()) || !std::isfinite(point.y())) return {};
    }
    return quad;
}

} // namespace okuflow::uct::windows_ocr

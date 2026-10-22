#include "okuflow/common/uct_paddle.hpp"
#include <algorithm>
#include <stdexcept>

// Exact function bodies extracted from 8f02fd11ed5ace89119b51d5cc2fb0576ea27e6d.
// Keep this reference independent of production sampling helpers.
namespace okuflow::uct::baseline8f02
{
QPointF At(const PaddleQuad &q, double x, double y)
{
    return q.points[0] * ((1 - x) * (1 - y)) + q.points[1] * (x * (1 - y)) + q.points[2] * (x * y) +
           q.points[3] * ((1 - x) * y);
}

float Sample(const QImage &image, float x, float y, int channel)
{
    x = std::clamp(x, 0.f, image.width() - 1.f);
    y = std::clamp(y, 0.f, image.height() - 1.f);
    const int ix = static_cast<int>(x), iy = static_cast<int>(y),
              jx = std::min(ix + 1, image.width() - 1), jy = std::min(iy + 1, image.height() - 1);
    const auto a = image.constScanLine(iy), b = image.constScanLine(jy);
    const float top = a[ix * 4 + channel] * (1 - (x - ix)) + a[jx * 4 + channel] * (x - ix);
    const float bottom = b[ix * 4 + channel] * (1 - (x - ix)) + b[jx * 4 + channel] * (x - ix);
    return top * (1 - (y - iy)) + bottom * (y - iy);
}

std::vector<float> PaddleNormalize(const QImage &image, QSize target, bool detection)
{
    if (image.isNull() || target.isEmpty() || target.width() > 4096 || target.height() > 4096)
        throw std::runtime_error("Invalid OCR image dimensions.");
    const QImage source = image.convertToFormat(QImage::Format_ARGB32);
    const std::size_t plane = static_cast<std::size_t>(target.width()) * target.height();
    std::vector<float> result(plane * 3);
    constexpr float mean[]{.485f, .456f, .406f}, deviation[]{.229f, .224f, .225f};
    for (int y = 0; y < target.height(); ++y)
    {
        for (int x = 0; x < target.width(); ++x)
            for (int channel = 0; channel < 3; ++channel)
            {
                const float sx = (x + .5f) * source.width() / target.width() - .5f;
                const float sy = (y + .5f) * source.height() / target.height() - .5f;
                const float v = Sample(source, sx, sy, channel) / 255.0f; // vendor BGR, not RGB
                result[channel * plane + static_cast<std::size_t>(y) * target.width() + x] =
                    detection ? (v - mean[channel]) / deviation[channel] : (v - .5f) * 2;
            }
    }
    return result;
}

std::vector<float> PaddleRectify(const QImage &image, const PaddleQuad &quad, int width,
                                 int validWidth)
{
    if (image.isNull() || width < 32 || width > 2048 || validWidth < 1)
        return {};
    const auto source = image.convertToFormat(QImage::Format_ARGB32);
    std::vector<float> crop(static_cast<std::size_t>(width) * 48 * 3, 0.f);
    const int valid = std::min(width, validWidth);
    for (int y = 0; y < 48; ++y)
        for (int x = 0; x < valid; ++x)
        {
            const auto p = At(quad, (x + .5f) / valid, (y + .5f) / 48);
            for (int ch = 0; ch < 3; ++ch)
            {
                crop[ch * width * 48 + y * width + x] =
                    (Sample(source, static_cast<float>(p.x()) - .5f,
                            static_cast<float>(p.y()) - .5f, ch) /
                         255 -
                     .5f) *
                    2;
            }
        }
    return crop;
}
} // namespace okuflow::uct::baseline8f02

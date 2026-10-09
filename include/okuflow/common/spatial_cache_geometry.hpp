#pragma once

#include "okuflow/common/view_transform.hpp"

#include <algorithm>
#include <cmath>

namespace okuflow {

struct SpatialCacheGeometry {
    unsigned int sourceX{}, sourceY{}, sourceWidth{}, sourceHeight{};
    unsigned int outputWidth{}, outputHeight{};
    bool valid{};
};

// Enclose the canonical visible ROI on integer source pixels. Use the existing
// scene workspace and cache allocation, cap work at 1440p, and respect NIS's
// maximum 2x enlargement per pass. Remaining display scaling stays in D3D12.
inline SpatialCacheGeometry ComputeSpatialCacheGeometry(
    const ViewTransform& view, unsigned int sceneWidth, unsigned int sceneHeight,
    unsigned int viewportWidth, unsigned int viewportHeight,
    unsigned int cacheWidth, unsigned int cacheHeight) {
    SpatialCacheGeometry result;
    if (!view.valid || !sceneWidth || !sceneHeight ||
        !viewportWidth || !viewportHeight || !cacheWidth || !cacheHeight ||
        !std::isfinite(view.sourceX) || !std::isfinite(view.sourceY) ||
        !std::isfinite(view.sourceWidth) || !std::isfinite(view.sourceHeight) ||
        !std::isfinite(view.destinationWidth) || !std::isfinite(view.destinationHeight) ||
        view.sourceX < 0 || view.sourceY < 0 || view.sourceWidth <= 0 ||
        view.sourceHeight <= 0 || view.destinationWidth <= 0 ||
        view.destinationHeight <= 0 ||
        view.sourceX + view.sourceWidth > 1.00001f ||
        view.sourceY + view.sourceHeight > 1.00001f) {
        return result;
    }
    result.sourceX = static_cast<unsigned int>(std::floor(view.sourceX * sceneWidth));
    result.sourceY = static_cast<unsigned int>(std::floor(view.sourceY * sceneHeight));
    const unsigned int right = std::min(sceneWidth, static_cast<unsigned int>(
        std::ceil((view.sourceX + view.sourceWidth) * sceneWidth)));
    const unsigned int bottom = std::min(sceneHeight, static_cast<unsigned int>(
        std::ceil((view.sourceY + view.sourceHeight) * sceneHeight)));
    if (right <= result.sourceX || bottom <= result.sourceY) {
        return {};
    }
    result.sourceWidth = right - result.sourceX;
    result.sourceHeight = bottom - result.sourceY;
    const unsigned int maxWidth = std::min({sceneWidth, cacheWidth,
        sceneWidth >= sceneHeight ? 2560u : 1440u});
    const unsigned int maxHeight = std::min({sceneHeight, cacheHeight,
        sceneWidth >= sceneHeight ? 1440u : 2560u});
    const double scale = std::min({
        2.0,
        static_cast<double>(viewportWidth) * view.destinationWidth /
            (view.sourceWidth * sceneWidth),
        static_cast<double>(viewportHeight) * view.destinationHeight /
            (view.sourceHeight * sceneHeight),
        static_cast<double>(maxWidth) / result.sourceWidth,
        static_cast<double>(maxHeight) / result.sourceHeight});
    if (scale <= 1.0) {
        return {};
    }
    result.outputWidth = static_cast<unsigned int>(std::floor(result.sourceWidth * scale));
    result.outputHeight = static_cast<unsigned int>(std::floor(result.sourceHeight * scale));
    result.valid = result.outputWidth > result.sourceWidth &&
                   result.outputHeight > result.sourceHeight;
    return result;
}

} // namespace okuflow

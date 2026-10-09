#include "okuflow/common/view_transform.hpp"

#include <algorithm>
#include <cmath>

namespace okuflow {

ViewTransform ComputeViewTransform(std::uint32_t sourceWidth,
                                   std::uint32_t sourceHeight,
                                   std::uint32_t viewportWidth,
                                   std::uint32_t viewportHeight,
                                   float zoom,
                                   float focusX,
                                   float focusY,
                                   ViewportFitMode mode) {
    ViewTransform result;
    if (sourceWidth == 0 || sourceHeight == 0 ||
        viewportWidth == 0 || viewportHeight == 0) {
        return result;
    }

    const float sourceWidthF = static_cast<float>(sourceWidth);
    const float sourceHeightF = static_cast<float>(sourceHeight);
    const float viewportWidthF = static_cast<float>(viewportWidth);
    const float viewportHeightF = static_cast<float>(viewportHeight);
    const float sourceAspect = sourceWidthF / sourceHeightF;
    const float viewportAspect = viewportWidthF / viewportHeightF;

    if (mode == ViewportFitMode::kFit) {
        const float uniformScale =
            std::min(viewportWidthF / sourceWidthF, viewportHeightF / sourceHeightF);
        const float activeWidth = sourceWidthF * uniformScale;
        const float activeHeight = sourceHeightF * uniformScale;
        result.destinationX = (viewportWidthF - activeWidth) * 0.5f / viewportWidthF;
        result.destinationY = (viewportHeightF - activeHeight) * 0.5f / viewportHeightF;
        result.destinationWidth = activeWidth / viewportWidthF;
        result.destinationHeight = activeHeight / viewportHeightF;
        result.valid = true;
        return result;
    }

    float cropWidth = sourceWidthF;
    float cropHeight = sourceHeightF;
    if (viewportAspect > sourceAspect) {
        cropHeight = sourceWidthF / viewportAspect;
    } else {
        cropWidth = sourceHeightF * viewportAspect;
    }

    const float safeZoom =
        std::max(1.0f, std::isfinite(zoom) ? zoom : 1.0f);
    const float sampleWidth = std::clamp(cropWidth / safeZoom, 1.0f, sourceWidthF);
    const float sampleHeight = std::clamp(cropHeight / safeZoom, 1.0f, sourceHeightF);
    const float centerX =
        std::clamp(focusX, 0.0f, 1.0f) * sourceWidthF;
    const float centerY =
        std::clamp(focusY, 0.0f, 1.0f) * sourceHeightF;
    const float originX =
        std::clamp(centerX - sampleWidth * 0.5f, 0.0f, sourceWidthF - sampleWidth);
    const float originY =
        std::clamp(centerY - sampleHeight * 0.5f, 0.0f, sourceHeightF - sampleHeight);

    result.sourceX = originX / sourceWidthF;
    result.sourceY = originY / sourceHeightF;
    result.sourceWidth = sampleWidth / sourceWidthF;
    result.sourceHeight = sampleHeight / sourceHeightF;
    result.valid = true;
    return result;
}

bool RemapViewTransformToSourceRect(
    const ViewTransform& fullSceneTransform,
    const NormalizedSourceRect& sourceRect,
    ViewTransform& remappedTransform) {
    remappedTransform = {};
    if (!fullSceneTransform.valid ||
        !std::isfinite(sourceRect.x) ||
        !std::isfinite(sourceRect.y) ||
        !std::isfinite(sourceRect.width) ||
        !std::isfinite(sourceRect.height) ||
        sourceRect.width <= 0.0f ||
        sourceRect.height <= 0.0f) {
        return false;
    }

    constexpr float kContainmentEpsilon = 1.0e-5f;
    const float sourceRight = sourceRect.x + sourceRect.width;
    const float sourceBottom = sourceRect.y + sourceRect.height;
    const float requestedRight =
        fullSceneTransform.sourceX + fullSceneTransform.sourceWidth;
    const float requestedBottom =
        fullSceneTransform.sourceY + fullSceneTransform.sourceHeight;
    if (fullSceneTransform.sourceX + kContainmentEpsilon < sourceRect.x ||
        fullSceneTransform.sourceY + kContainmentEpsilon < sourceRect.y ||
        requestedRight > sourceRight + kContainmentEpsilon ||
        requestedBottom > sourceBottom + kContainmentEpsilon) {
        return false;
    }

    remappedTransform = fullSceneTransform;
    remappedTransform.sourceX =
        (fullSceneTransform.sourceX - sourceRect.x) / sourceRect.width;
    remappedTransform.sourceY =
        (fullSceneTransform.sourceY - sourceRect.y) / sourceRect.height;
    remappedTransform.sourceWidth =
        fullSceneTransform.sourceWidth / sourceRect.width;
    remappedTransform.sourceHeight =
        fullSceneTransform.sourceHeight / sourceRect.height;
    remappedTransform.sourceX =
        std::clamp(remappedTransform.sourceX, 0.0f, 1.0f);
    remappedTransform.sourceY =
        std::clamp(remappedTransform.sourceY, 0.0f, 1.0f);
    remappedTransform.sourceWidth =
        std::clamp(remappedTransform.sourceWidth,
                   0.0f,
                   1.0f - remappedTransform.sourceX);
    remappedTransform.sourceHeight =
        std::clamp(remappedTransform.sourceHeight,
                   0.0f,
                   1.0f - remappedTransform.sourceY);
    return true;
}

PixelViewMapping ComputePixelViewMapping(
    const ViewTransform& transform,
    std::uint32_t sourceWidth,
    std::uint32_t sourceHeight,
    std::uint32_t viewportWidth,
    std::uint32_t viewportHeight) {
    PixelViewMapping result;
    if (!transform.valid || sourceWidth == 0 || sourceHeight == 0 ||
        viewportWidth == 0 || viewportHeight == 0) {
        return result;
    }

    result.targetWidth = viewportWidth;
    result.targetHeight = viewportHeight;
    result.offsetX = std::min(
        viewportWidth - 1,
        static_cast<std::uint32_t>(std::lround(
            transform.destinationX * static_cast<float>(viewportWidth))));
    result.offsetY = std::min(
        viewportHeight - 1,
        static_cast<std::uint32_t>(std::lround(
            transform.destinationY * static_cast<float>(viewportHeight))));
    result.activeWidth = std::clamp(
        static_cast<std::uint32_t>(std::lround(
            transform.destinationWidth * static_cast<float>(viewportWidth))),
        1u,
        viewportWidth - result.offsetX);
    result.activeHeight = std::clamp(
        static_cast<std::uint32_t>(std::lround(
            transform.destinationHeight * static_cast<float>(viewportHeight))),
        1u,
        viewportHeight - result.offsetY);
    result.startX =
        transform.sourceX * static_cast<float>(sourceWidth);
    result.startY =
        transform.sourceY * static_cast<float>(sourceHeight);
    result.stepX =
        transform.sourceWidth * static_cast<float>(sourceWidth) /
        static_cast<float>(result.activeWidth);
    result.stepY =
        transform.sourceHeight * static_cast<float>(sourceHeight) /
        static_cast<float>(result.activeHeight);
    result.valid =
        std::isfinite(result.startX) && std::isfinite(result.startY) &&
        std::isfinite(result.stepX) && std::isfinite(result.stepY) &&
        result.stepX > 0.0f && result.stepY > 0.0f;
    return result;
}

} // namespace okuflow

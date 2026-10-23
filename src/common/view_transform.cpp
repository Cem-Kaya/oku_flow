#include "okuflow/common/view_transform.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace okuflow {

float ComputeFitZoom(std::uint32_t sourceWidth, std::uint32_t sourceHeight,
                     std::uint32_t viewportWidth, std::uint32_t viewportHeight) {
    if (!sourceWidth || !sourceHeight || !viewportWidth || !viewportHeight) return 1.0f;
    const double x = static_cast<double>(viewportWidth) / sourceWidth;
    const double y = static_cast<double>(viewportHeight) / sourceHeight;
    return static_cast<float>(std::min(x, y) / std::max(x, y));
}

float EffectiveFillZoom(float requestedZoom, float fitZoom) {
    if (!std::isfinite(fitZoom) || fitZoom <= 0.0f || fitZoom > 1.0f) fitZoom = 1.0f;
    if (!std::isfinite(requestedZoom) || requestedZoom <= 0.0f) requestedZoom = 1.0f;
    return requestedZoom <= kWholeSourceZoom ? fitZoom : std::max(fitZoom, requestedZoom);
}

float StepFillZoom(float currentZoom, float notches, float acceleration, float fitZoom) {
    const float current = EffectiveFillZoom(currentZoom, fitZoom);
    if (!std::isfinite(notches) || !std::isfinite(acceleration)) return current;
    const float target = current * std::pow(1.10f, notches * acceleration);
    if ((current < 1.0f && target >= 1.0f) || (current > 1.0f && target <= 1.0f)) return 1.0f;
    return target <= fitZoom ? kWholeSourceZoom : target;
}

float FillZoomFromTransform(const ViewTransform& transform) {
    if (!transform.valid || transform.sourceWidth <= 0 || transform.sourceHeight <= 0 ||
        transform.destinationWidth <= 0 || transform.destinationHeight <= 0)
        return 0.0f;
    // At 1x Fill one source axis spans the full source. Its normalized
    // destination/source ratio is therefore the smaller ratio, even with
    // partial bars. Invert the committed transform without borrowing a newer
    // viewport size while asynchronous Freeze readback is pending.
    const float x=transform.destinationWidth/transform.sourceWidth;
    const float y=transform.destinationHeight/transform.sourceHeight;
    return std::isfinite(x) && std::isfinite(y) ? std::min(x,y) : 0.0f;
}

bool ComputeZoomAnchorCenter(const ViewTransform& before, const ViewTransform& after,
                             float anchorX, float anchorY, float& centerX, float& centerY) {
    if (!before.valid || !after.valid || !std::isfinite(anchorX) || !std::isfinite(anchorY) ||
        before.sourceWidth <= 0 || before.sourceHeight <= 0 || after.sourceWidth <= 0 || after.sourceHeight <= 0 ||
        before.destinationWidth <= 0 || before.destinationHeight <= 0 || after.destinationWidth <= 0 || after.destinationHeight <= 0)
        return false;
    const float oldU = (anchorX-before.sourceX)/before.sourceWidth;
    const float oldV = (anchorY-before.sourceY)/before.sourceHeight;
    if (oldU < 0 || oldU > 1 || oldV < 0 || oldV > 1) return false;
    const float viewportX = before.destinationX+oldU*before.destinationWidth;
    const float viewportY = before.destinationY+oldV*before.destinationHeight;
    const float newU = (viewportX-after.destinationX)/after.destinationWidth;
    const float newV = (viewportY-after.destinationY)/after.destinationHeight;
    const float x = anchorX-(newU-0.5f)*after.sourceWidth;
    const float y = anchorY-(newV-0.5f)*after.sourceHeight;
    if (!std::isfinite(x) || !std::isfinite(y)) return false;
    centerX = std::clamp(x,0.0f,1.0f); centerY = std::clamp(y,0.0f,1.0f);
    return true;
}

ViewTransform CanonicalizeSuperResViewTransform(const ViewTransform& requested,
                                               std::uint32_t sceneWidth,
                                               std::uint32_t sceneHeight,
                                               std::uint32_t outputWidth,
                                               std::uint32_t outputHeight,
                                               float zoom) {
    if (!requested.valid || !sceneWidth || !sceneHeight || !outputWidth || !outputHeight) return requested;
    for (const auto scale : {std::pair{4u, 1u}, std::pair{3u, 1u},
                             std::pair{2u, 1u}, std::pair{3u, 2u}, std::pair{4u, 3u}}) {
        const auto [num, den] = scale;
        if (zoom + 0.005f < static_cast<float>(num) / den ||
            (outputWidth * den) % num || (outputHeight * den) % num) continue;
        const auto cropWidth = outputWidth * den / num;
        const auto cropHeight = outputHeight * den / num;
        if (cropWidth > sceneWidth || cropHeight > sceneHeight ||
            cropWidth < 160 || cropHeight < 90 || (cropWidth & 1) || (cropHeight & 1)) continue;
        const float cropW = static_cast<float>(cropWidth) / sceneWidth;
        const float cropH = static_cast<float>(cropHeight) / sceneHeight;
        if (requested.sourceWidth > cropW + 0.005f || requested.sourceHeight > cropH + 0.005f) continue;
        ViewTransform result = requested;
        const float extentScale = std::min({1.0f, cropW / result.sourceWidth,
                                           cropH / result.sourceHeight});
        result.sourceWidth *= extentScale;
        result.sourceHeight *= extentScale;
        const auto snap = [](float origin, float extent, float crop, std::uint32_t scene) {
            const float cropOrigin = std::clamp(std::round((origin + (extent - crop) * 0.5f) * scene),
                                              0.0f, std::round((1.0f - crop) * scene)) / scene;
            // Float subtraction can put the upper bound slightly below the
            // lower bound when extent equals crop (notably denominators720).
            const float maximumOrigin = std::max(cropOrigin, cropOrigin + (crop - extent));
            return std::clamp(origin, cropOrigin, maximumOrigin);
        };
        result.sourceX = snap(result.sourceX, result.sourceWidth, cropW, sceneWidth);
        result.sourceY = snap(result.sourceY, result.sourceHeight, cropH, sceneHeight);
        return result;
    }
    return requested;
}

bool ComputeContainingSourceCrop(const ViewTransform& requested,
                                 std::uint32_t sceneWidth,
                                 std::uint32_t sceneHeight,
                                 std::uint32_t cropWidth,
                                 std::uint32_t cropHeight,
                                 std::uint32_t& originX,
                                 std::uint32_t& originY) {
    if (!requested.valid || !sceneWidth || !sceneHeight || !cropWidth ||
        !cropHeight || cropWidth > sceneWidth || cropHeight > sceneHeight) {
        return false;
    }
    const auto axis = [](float start, float extent, std::uint32_t scene,
                         std::uint32_t crop, std::uint32_t& origin) {
        if (!std::isfinite(start) || !std::isfinite(extent) ||
            start < 0.0f || extent <= 0.0f || start + extent > 1.00001f) {
            return false;
        }
        // Match RemapViewTransformToSourceRect's normalized tolerance.
        const double epsilon = 1.0e-5 * scene;
        const double left = static_cast<double>(start) * scene;
        const double right = static_cast<double>(start + extent) * scene;
        const double minimum = std::max(0.0, std::ceil(right - crop - epsilon));
        const double maximum = std::min(static_cast<double>(scene - crop),
                                        std::floor(left + epsilon));
        if (minimum > maximum) return false;
        origin = static_cast<std::uint32_t>(std::clamp(
            std::round((left + right - crop) * 0.5), minimum, maximum));
        return true;
    };
    return axis(requested.sourceX, requested.sourceWidth, sceneWidth, cropWidth, originX) &&
           axis(requested.sourceY, requested.sourceHeight, sceneHeight, cropHeight, originY);
}

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

    const float safeZoom = EffectiveFillZoom(zoom,
        ComputeFitZoom(sourceWidth, sourceHeight, viewportWidth, viewportHeight));
    // When an axis has no more source pixels to reveal, shrink its centered
    // destination instead of stretching the independently clamped crop.
    if (safeZoom < 1.0f) {
        result.destinationWidth = std::min(1.0f, sourceWidthF * safeZoom / cropWidth);
        result.destinationHeight = std::min(1.0f, sourceHeightF * safeZoom / cropHeight);
        result.destinationX = (1.0f - result.destinationWidth) * 0.5f;
        result.destinationY = (1.0f - result.destinationHeight) * 0.5f;
    }
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

#pragma once

#include <cstdint>

namespace okuflow {

enum class ViewportFitMode : std::uint8_t {
    kFill,
    kFit,
};

// One geometry contract for camera-to-viewport mapping. Source and destination
// rectangles are normalized so the same result can drive D3D presentation,
// pointer mapping, overlays, and captures without independent X/Y scaling.
struct ViewTransform {
    float sourceX{};
    float sourceY{};
    float sourceWidth{1.0f};
    float sourceHeight{1.0f};
    float destinationX{};
    float destinationY{};
    float destinationWidth{1.0f};
    float destinationHeight{1.0f};
    bool valid{};
};

struct NormalizedSourceRect {
    float x{};
    float y{};
    float width{1.0f};
    float height{1.0f};
};

// Integer destination bounds and source sampling increments derived from a
// ViewTransform. CPU fallback presentation uses this instead of maintaining a
// second aspect/crop implementation.
struct PixelViewMapping {
    std::uint32_t targetWidth{};
    std::uint32_t targetHeight{};
    std::uint32_t activeWidth{};
    std::uint32_t activeHeight{};
    std::uint32_t offsetX{};
    std::uint32_t offsetY{};
    float startX{};
    float startY{};
    float stepX{};
    float stepY{};
    bool valid{};
};

ViewTransform ComputeViewTransform(std::uint32_t sourceWidth,
                                   std::uint32_t sourceHeight,
                                   std::uint32_t viewportWidth,
                                   std::uint32_t viewportHeight,
                                   float zoom,
                                   float focusX,
                                   float focusY,
                                   ViewportFitMode mode);

bool RemapViewTransformToSourceRect(const ViewTransform& fullSceneTransform,
                                    const NormalizedSourceRect& sourceRect,
                                    ViewTransform& remappedTransform);

PixelViewMapping ComputePixelViewMapping(const ViewTransform& transform,
                                         std::uint32_t sourceWidth,
                                         std::uint32_t sourceHeight,
                                         std::uint32_t viewportWidth,
                                         std::uint32_t viewportHeight);

} // namespace okuflow

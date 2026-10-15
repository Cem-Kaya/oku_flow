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

// Inverts the actual cropped/letterboxed transforms to preserve the viewport
// location of a visible scene anchor when magnification changes.
bool ComputeZoomAnchorCenter(const ViewTransform& before, const ViewTransform& after,
                             float anchorX, float anchorY, float& centerX, float& centerY);

struct NormalizedSourceRect {
    float x{};
    float y{};
    float width{1.0f};
    float height{1.0f};
};

// Finds an integer camera crop that contains the entire requested viewport.
// Returns false when SDK crop dimensions cannot cover fractional boundaries.
bool ComputeContainingSourceCrop(const ViewTransform& requested,
                                 std::uint32_t sceneWidth,
                                 std::uint32_t sceneHeight,
                                 std::uint32_t cropWidth,
                                 std::uint32_t cropHeight,
                                 std::uint32_t& originX,
                                 std::uint32_t& originY);

// SR-only viewport contract. Snaps at most one source pixel of pan and the
// advertised 1.33 scale to exact 4/3; callers use this for both inference and
// presentation. Ordinary/non-SR transforms must not call this helper.
ViewTransform CanonicalizeSuperResViewTransform(const ViewTransform& requested,
                                               std::uint32_t sceneWidth,
                                               std::uint32_t sceneHeight,
                                               std::uint32_t outputWidth,
                                               std::uint32_t outputHeight,
                                               float zoom);

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

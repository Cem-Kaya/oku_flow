#include "okuflow/common/spatial_cache_geometry.hpp"
#include "okuflow/common/superres_scale_policy.hpp"

#include <iostream>
#include <limits>
#include <utility>

int main() {
    using namespace okuflow;
    bool passed = true;
    auto check = [&](bool condition, const char* label) {
        if (!condition) {
            passed = false;
            std::cerr << label << '\n';
        }
    };
    // A 4000x2256 phone frame at 16x used to choose a 1000x564 ->
    // 4000x2256 4x model, which the real SDK rejects with resolution error -16.
    // Keep desktop zoom independent and allow its valid 2000x1128 2x crop.
    check(!IsSuperResInputHeightSupported(2256u / 4u, 4u, 1u),
          "phone 4x crop exceeds NVIDIA's 540p model limit");
    check(IsSuperResInputHeightSupported(2256u / 2u, 2u, 1u),
          "phone 2x crop must remain eligible at high viewport zoom");
    check(IsSuperResInputHeightSupported(540u, 4u, 1u) &&
          !IsSuperResInputHeightSupported(542u, 4u, 1u),
          "4x model boundary must preserve 2160p output");
    check(IsSuperResInputHeightSupported(720u, 3u, 1u) &&
          !IsSuperResInputHeightSupported(722u, 3u, 1u),
          "3x model boundary must preserve 2160p output");
    for (const auto scale : {std::pair{2u, 1u}, std::pair{3u, 2u},
                             std::pair{4u, 3u}}) {
        check(IsSuperResInputHeightSupported(2160u, scale.first, scale.second) &&
              !IsSuperResInputHeightSupported(2162u, scale.first, scale.second),
              "lower-factor model input must stay within 2160p");
    }
    check(!IsSuperResInputHeightSupported(88u, 2u, 1u) &&
          !IsSuperResInputHeightSupported(540u, 5u, 1u),
          "undersized input and unknown factors must be rejected");
    for (const float zoom : {1.33f, 2.0f, 4.0f, 16.0f}) {
        for (const float focus : {0.0f, 0.37f, 1.0f}) {
            const auto view = ComputeViewTransform(
                1280, 720, 1280, 720, zoom, focus, focus, ViewportFitMode::kFill);
            const auto geometry = ComputeSpatialCacheGeometry(
                view, 1280, 720, 1280, 720, 1280, 720);
            check(geometry.valid, "magnified view must use a spatial upscale");
            check(geometry.outputWidth > geometry.sourceWidth &&
                  geometry.outputHeight > geometry.sourceHeight,
                  "backend must receive different source/output dimensions");
            check(geometry.outputWidth <= 2 * geometry.sourceWidth &&
                  geometry.outputHeight <= 2 * geometry.sourceHeight,
                  "NIS per-pass 2x limit exceeded");
            check(geometry.outputWidth <= 1280 && geometry.outputHeight <= 720,
                  "output exceeds reusable workspace");
            ViewTransform remapped;
            check(RemapViewTransformToSourceRect(view,
                {geometry.sourceX / 1280.0f, geometry.sourceY / 720.0f,
                 geometry.sourceWidth / 1280.0f, geometry.sourceHeight / 720.0f},
                remapped), "rounded cache must contain canonical view");
            // Sampling uses the physical allocation, including when only its
            // top-left region contains the 2x cache at higher viewport zoom.
            const float occupiedX = geometry.outputWidth / 1280.0f;
            const float occupiedY = geometry.outputHeight / 720.0f;
            check((remapped.sourceX + remapped.sourceWidth) * occupiedX <=
                      occupiedX + 1e-5f &&
                  (remapped.sourceY + remapped.sourceHeight) * occupiedY <=
                      occupiedY + 1e-5f,
                  "presentation samples outside initialized cache pixels");
        }
    }
    const auto native = ComputeViewTransform(
        1280, 720, 1280, 720, 1, 0.5f, 0.5f, ViewportFitMode::kFill);
    check(!ComputeSpatialCacheGeometry(native, 1280, 720, 1280, 720,
                                       1280, 720).valid,
          "native view should retain full-scene sharpening");
    const auto fit = ComputeViewTransform(
        1280, 720, 800, 800, 4, 0.5f, 0.5f, ViewportFitMode::kFit);
    check(!ComputeSpatialCacheGeometry(fit, 1280, 720, 800, 800,
                                       1280, 720).valid,
          "Fit view must not use hidden requested zoom");
    const auto largeView = ComputeViewTransform(
        7680, 4320, 3840, 2160, 4, 0.5f, 0.5f, ViewportFitMode::kFill);
    const auto bounded = ComputeSpatialCacheGeometry(
        largeView, 7680, 4320, 3840, 2160, 7680, 4320);
    check(bounded.valid && bounded.outputWidth <= 2560 &&
          bounded.outputHeight <= 1440, "landscape cache must stay within 1440p");
    const auto portraitView = ComputeViewTransform(
        4320, 7680, 2160, 3840, 4, 0.5f, 0.5f, ViewportFitMode::kFill);
    const auto portrait = ComputeSpatialCacheGeometry(
        portraitView, 4320, 7680, 2160, 3840, 4320, 7680);
    check(portrait.valid && portrait.outputWidth <= 1440 &&
          portrait.outputHeight <= 2560, "portrait cache must stay within 1440p");
    auto invalid = native;
    invalid.sourceWidth = std::numeric_limits<float>::quiet_NaN();
    check(!ComputeSpatialCacheGeometry(invalid, 1280, 720, 1280, 720,
                                       1280, 720).valid,
          "nonfinite viewport must not launch kernels");
    check(!ComputeSpatialCacheGeometry(native, 0, 720, 1280, 720,
                                       1280, 720).valid,
          "empty source must not launch kernels");
    return passed ? 0 : 1;
}
